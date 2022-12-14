// Copyright (c) 2019-2021 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "electrum/rostrum.h"
#include "extversionkeys.h"
#include "extversionmessage.h"
#include "test/test_nexa.h"
#include "util.h"

#include <sstream>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

using namespace electrum;

BOOST_FIXTURE_TEST_SUITE(rostrum_tests, BasicTestingSetup)

static bool rostrum_args_has(const std::string &arg, const std::string &network = "nexa")
{
    const std::vector<std::string> args = rostrum_args(42, network);
    return std::find(begin(args), end(args), arg) != end(args);
}

// Test case for github issue #1700
BOOST_AUTO_TEST_CASE(issue_1700)
{
    UnsetArg("-electrum.port");
    SetArg("-electrum.host", "foo");
    BOOST_CHECK(rostrum_args_has("--electrum-rpc-addr=foo:20001"));

    UnsetArg("-electrum.host");
    SetArg("-electrum.port", "24");
    BOOST_CHECK(rostrum_args_has("--electrum-rpc-addr=0.0.0.0:24"));

    SetArg("-electrum.port", "24");
    SetArg("-electrum.host", "foo");
    BOOST_CHECK(rostrum_args_has("--electrum-rpc-addr=foo:24"));

    UnsetArg("-electrum.host");
    UnsetArg("-electrum.port");
    BOOST_CHECK(rostrum_args_has("--electrum-rpc-addr=0.0.0.0:20001"));
    BOOST_CHECK(rostrum_args_has("--electrum-rpc-addr=0.0.0.0:30001", "testnet"));
}

BOOST_AUTO_TEST_CASE(rawargs)
{
    BOOST_CHECK(rostrum_args_has("--network=bitcoin"));
    BOOST_CHECK(!rostrum_args_has("--network=scalenet"));

    // Test that we override network and append server-banner
    mapMultiArgs["-electrum.rawarg"].push_back("--network=scalenet");
    mapMultiArgs["-electrum.rawarg"].push_back("--server-banner=\"Hello World!\"");

    BOOST_CHECK(!rostrum_args_has("--network=bitcoin"));
    BOOST_CHECK(rostrum_args_has("--network=scalenet"));
    BOOST_CHECK(rostrum_args_has("--server-banner=\"Hello World!\""));

    mapMultiArgs.clear();
}

BOOST_AUTO_TEST_CASE(rawargs_verboseness)
{
    Logging::LogToggleCategory(ELECTRUM, true);
    BOOST_CHECK(rostrum_args_has("-vvvv"));
    BOOST_CHECK(!rostrum_args_has("-v"));

    mapMultiArgs["-electrum.rawarg"].push_back("-v");
    BOOST_CHECK(!rostrum_args_has("-vvvv"));
    BOOST_CHECK(rostrum_args_has("-v"));

    mapMultiArgs["-electrum.rawarg"].push_back("-vv");
    BOOST_CHECK(!rostrum_args_has("-vvvv"));
    BOOST_CHECK(rostrum_args_has("-vv"));
    mapMultiArgs.clear();
    Logging::LogToggleCategory(ELECTRUM, false);
}

static void call_setter(std::unique_ptr<CExtversionMessage> &ver)
{
    constexpr char network[] = "nexa";
    ver.reset(new CExtversionMessage);
    set_extversion_flags(*ver, network);
}

BOOST_AUTO_TEST_CASE(electrum_extversion)
{
    constexpr uint64_t PORT = 2020;
    constexpr uint64_t WS_PORT = 2021;
    constexpr uint64_t NOT_SET = 0;

    UnsetArg("-electrum");
    UnsetArg("-electrum.host");
    UnsetArg("-electrum.ws.host");
    std::stringstream ss;
    ss << PORT;
    SetArg("-electrum.port", ss.str());
    std::stringstream ss2;
    ss2 << WS_PORT;
    SetArg("-electrum.ws.port", ss2.str());

    std::unique_ptr<CExtversionMessage> ver;

    // Electrum server not enabled
    call_setter(ver);
    BOOST_CHECK_EQUAL(NOT_SET, ver->as_u64c(XVer::BU_ELECTRUM_SERVER_PORT_TCP));
    BOOST_CHECK_EQUAL(NOT_SET, ver->as_u64c(XVer::BU_ELECTRUM_WS_SERVER_PORT_TCP));
    BOOST_CHECK_EQUAL(NOT_SET, ver->as_u64c(XVer::BU_ELECTRUM_SERVER_PROTOCOL_VERSION));

    // Electrum server enabled, but host is localhost
    SetArg("-electrum", "1");
    SetArg("-electrum.host", "127.0.0.1");
    SetArg("-electrum.ws.host", "127.0.0.1");
    call_setter(ver);
    BOOST_CHECK_EQUAL(NOT_SET, ver->as_u64c(XVer::BU_ELECTRUM_SERVER_PORT_TCP));
    BOOST_CHECK_EQUAL(NOT_SET, ver->as_u64c(XVer::BU_ELECTRUM_WS_SERVER_PORT_TCP));
    BOOST_CHECK_EQUAL(NOT_SET, ver->as_u64c(XVer::BU_ELECTRUM_SERVER_PROTOCOL_VERSION));

    // Electrum server enabled, but host is private network
    SetArg("-electrum.host", "192.168.1.42");
    SetArg("-electrum.ws.host", "192.168.1.42");
    call_setter(ver);
    BOOST_CHECK_EQUAL(NOT_SET, ver->as_u64c(XVer::BU_ELECTRUM_SERVER_PORT_TCP));
    BOOST_CHECK_EQUAL(NOT_SET, ver->as_u64c(XVer::BU_ELECTRUM_WS_SERVER_PORT_TCP));
    BOOST_CHECK_EQUAL(NOT_SET, ver->as_u64c(XVer::BU_ELECTRUM_SERVER_PROTOCOL_VERSION));

    // Electrum server enabled and on public network
    SetArg("-electrum.host", "8.8.8.8");
    SetArg("-electrum.ws.host", "1.1.1.1");
    call_setter(ver);
    BOOST_CHECK_EQUAL(PORT, ver->as_u64c(XVer::BU_ELECTRUM_SERVER_PORT_TCP));
    BOOST_CHECK_EQUAL(WS_PORT, ver->as_u64c(XVer::BU_ELECTRUM_WS_SERVER_PORT_TCP));
    BOOST_CHECK_EQUAL(1040300ULL, ver->as_u64c(XVer::BU_ELECTRUM_SERVER_PROTOCOL_VERSION));

    // Special case: Listen on all IP's is treated as public
    SetArg("-electrum.host", "0.0.0.0");
    SetArg("-electrum.ws.host", "0.0.0.0");
    call_setter(ver);
    BOOST_CHECK_EQUAL(PORT, ver->as_u64c(XVer::BU_ELECTRUM_SERVER_PORT_TCP));
    BOOST_CHECK_EQUAL(WS_PORT, ver->as_u64c(XVer::BU_ELECTRUM_WS_SERVER_PORT_TCP));
}

// Test case for gitlab issue #2221, passing boolean parameters did not work.
BOOST_AUTO_TEST_CASE(issue_2221)
{
    mapMultiArgs["-electrum.rawarg"].push_back("--disable-full-compaction");
    mapMultiArgs["-electrum.rawarg"].push_back("--jsonrpc-import");
    BOOST_CHECK(rostrum_args_has("--disable-full-compaction"));
    BOOST_CHECK(rostrum_args_has("--jsonrpc-import"));
}

BOOST_AUTO_TEST_SUITE_END()
