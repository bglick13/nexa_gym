// Copyright (c) 2019-2021 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include "electrum/rostrum.h"
#include "chainparamsbase.h"
#include "extversionkeys.h"
#include "extversionmessage.h"
#include "netaddress.h"
#include "util.h"
#include "utilhttp.h"
#include "utilprocess.h"

#include <map>
#include <regex>
#include <sstream>

#include <boost/filesystem.hpp>

constexpr char ROSTRUM_BIN[] = "rostrum";

static std::string monitoring_port() { return GetArg("-electrum.monitoring.port", "3224"); }
static std::string monitoring_host() { return GetArg("-electrum.monitoring.host", "127.0.0.1"); }
static std::string rpc_host() { return GetArg("-electrum.host", "0.0.0.0"); }
static std::string rpc_port(const std::string &network)
{
    std::map<std::string, std::string> portmap = {{"nexa", "20001"}, {"testnet", "30001"}, {"regtest", "30403"}};

    auto defaultPort = portmap.find(network);
    if (defaultPort == end(portmap))
    {
        std::stringstream ss;
        ss << "Electrum server does not support '" << network << "' network.";
        throw std::invalid_argument(ss.str());
    }

    return GetArg("-electrum.port", defaultPort->second);
}
static std::string ws_host() { return GetArg("-electrum.ws.host", "0.0.0.0"); }
static std::string ws_port(const std::string &network)
{
    const std::map<std::string, std::string> portmap = {{"nexa", "20003"}, {"testnet", "30003"}, {"regtest", "30404"}};

    auto defaultPort = portmap.find(network);
    if (defaultPort == end(portmap))
    {
        std::stringstream ss;
        ss << "Electrum server does not support '" << network << "' network.";
        throw std::invalid_argument(ss.str());
    }
    return GetArg("-electrum.ws.port", defaultPort->second);
}

static bool is_public_host(const std::string &host)
{
    // Special case, CNetAddr treats "0.0.0.0" as local, but rostrum
    // treats it as listen on all IPs.
    if (host == "0.0.0.0")
    {
        return true;
    }

    // Assume the server is public if it's not listening on localhost and
    // not listening on a private network (RFC1918)
    const CNetAddr listenaddr(host);

    return !listenaddr.IsLocal() && !listenaddr.IsRFC1918();
}

static void remove_conflicting_arg(std::vector<std::string> &args, const std::string &override_arg)
{
    // special case: verboseness argument
    const std::regex verbose("^-v+$");
    if (std::regex_search(override_arg, verbose))
    {
        auto it = begin(args);
        while (it != end(args))
        {
            if (!std::regex_search(*it, verbose))
            {
                ++it;
                continue;
            }
            LOGA("Electrum: Argument '%s' overrides '%s'", override_arg, *it);
            it = args.erase(it);
        }
        return;
    }

    // normal case
    auto separator = override_arg.find_first_of("=");
    if (separator == std::string::npos)
    {
        // switch flag, for example "--disable-full-compaction".
        auto it = begin(args);
        while (it != end(args))
        {
            if (*it != override_arg)
            {
                ++it;
                continue;
            }
            // Remove duplicate.
            it = args.erase(it);
        }
        return;
    }
    separator++; // include '=' when matching argument names below

    auto it = begin(args);
    while (it != end(args))
    {
        if (it->size() < separator)
        {
            ++it;
            continue;
        }
        if (it->substr(0, separator) != override_arg.substr(0, separator))
        {
            ++it;
            continue;
        }
        LOGA("Electrum: Argument '%s' overrides '%s'", override_arg, *it);
        it = args.erase(it);
    }
}
namespace electrum
{
std::string rostrum_path()
{
    // look for rostrum in same path as nexad
    boost::filesystem::path nexad_dir(this_process_path());
    nexad_dir = nexad_dir.remove_filename();

    auto default_path = nexad_dir / ROSTRUM_BIN;
    const std::string path = GetArg("-electrum.exec", default_path.string());

    if (path.empty())
    {
        throw std::runtime_error("Path to electrum server executable not found. "
                                 "You can specify full path with -electrum.exec");
    }
    if (!boost::filesystem::exists(path))
    {
        std::stringstream ss;
        ss << "Cannot find electrum executable at " << path;
        throw std::runtime_error(ss.str());
    }
    return path;
}

//! Arguments to start rostrum server with
std::vector<std::string> rostrum_args(int rpcport, const std::string &network)
{
    std::vector<std::string> args;

    if (Logging::LogAcceptCategory(ELECTRUM))
    {
        // increase verboseness when electrum logging is enabled
        args.push_back("-vvvv");
    }

    // address to nexad rpc interface
    {
        rpcport = GetArg("-rpcport", rpcport);
        std::stringstream ss;
        ss << "--daemon-rpc-addr=" << GetArg("-electrum.daemon.host", "127.0.0.1") << ":" << rpcport;
        args.push_back(ss.str());
    }

    args.push_back("--electrum-rpc-addr=" + rpc_host() + ":" + rpc_port(network));
    args.push_back("--electrum-ws-addr=" + ws_host() + ":" + ws_port(network));

    // nexad data dir (for cookie file)
    args.push_back("--daemon-dir=" + GetDataDir(false).string());

    // Where to store rostrum database files.
    const std::string defaultDir = (GetDataDir() / ROSTRUM_BIN).string();
    args.push_back("--db-dir=" + GetArg("-electrum.dir", defaultDir));

    // Tell rostrum what network we're on
    const std::map<std::string, std::string> netmapping = {
        {"nexa", "bitcoin"}, {"testnet", "testnet"}, {"regtest", "regtest"}};
    if (!netmapping.count(network))
    {
        std::stringstream ss;
        ss << "Electrum server does not support '" << network << "' network.";
        throw std::invalid_argument(ss.str());
    }
    args.push_back("--network=" + netmapping.at(network));
    args.push_back("--monitoring-addr=" + monitoring_host() + ":" + monitoring_port());

    if (!GetArg("-rpcpassword", "").empty())
    {
        args.push_back("--cookie=" + GetArg("-rpcuser", "") + ":" + GetArg("-rpcpassword", ""));
    }
    else
    {
        // This explicit code ought to work for any network, but it is only needed for NEXA because electrs
        // guesses "testnet3" since we told it testnet was being used.
        if (network == CBaseChainParams::NEXA)
        {
            args.push_back("--cookie-file=" + (GetDataDir() / ".cookie").string());
        }
    }

    for (auto &a : mapMultiArgs["-electrum.rawarg"])
    {
        remove_conflicting_arg(args, a);
        args.push_back(a);
    }

    return args;
}

std::map<std::string, int64_t> fetch_rostrum_info()
{
    if (!GetBoolArg("-electrum", false))
    {
        throw std::runtime_error("Electrum server is disabled");
    }

    std::stringstream infostream = http_get(monitoring_host(), std::stoi(monitoring_port()), "/");

    const std::regex keyval("^([a-z_{}=\"\\+]+)\\s(\\d+)\\s*$");
    std::map<std::string, int64_t> info;
    std::string line;
    std::smatch match;
    while (std::getline(infostream, line, '\n'))
    {
        if (!std::regex_match(line, match, keyval))
        {
            continue;
        }
        try
        {
            info[match[1].str()] = std::stol(match[2].str());
        }
        catch (const std::exception &e)
        {
            LOG(ELECTRUM, "%s error: %s", __func__, e.what());
        }
    }
    return info;
}

void set_extversion_flags(CExtversionMessage &xver, const std::string &network)
{
    if (!GetBoolArg("-electrum", false))
    {
        return;
    }

    // Electrum protocol version 1.4.3
    constexpr uint64_t major = 1;
    constexpr uint64_t minor = 4;
    constexpr uint64_t revision = 3;

    const uint64_t electrum_protocol_version = 1000000 * major + 10000 * minor + 100 * revision;

    if (is_public_host(rpc_host()))
    {
        xver.set_u64c(XVer::BU_ELECTRUM_SERVER_PORT_TCP, std::stoul(rpc_port(network)));
        xver.set_u64c(XVer::BU_ELECTRUM_SERVER_PROTOCOL_VERSION, electrum_protocol_version);
    }
    if (is_public_host(ws_host()))
    {
        xver.set_u64c(XVer::BU_ELECTRUM_WS_SERVER_PORT_TCP, std::stoul(ws_port(network)));
        xver.set_u64c(XVer::BU_ELECTRUM_SERVER_PROTOCOL_VERSION, electrum_protocol_version);
    }
}
} // namespace electrum
