// Copyright (c) 2015 G. Andrew Stone
// Copyright (c) 2015-2022 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "unlimited.h"

#include "base58.h"
#include "blockrelay/graphene.h"
#include "blockrelay/thinblock.h"
#include "blockstorage/blockstorage.h"
#include "cashaddrenc.h"
#include "chain.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "connmgr.h"
#include "consensus/consensus.h"
#include "consensus/merkle.h"
#include "consensus/params.h"
#include "consensus/validation.h"
#include "core_io.h"
#include "dosman.h"
#include "dstencode.h"
#include "expedited.h"
#include "hashwrapper.h"
#include "init.h"
#include "leakybucket.h"
#include "miner.h"
#include "net.h"
#include "policy/policy.h"
#include "primitives/block.h"
#include "requestManager.h"
#include "rpc/server.h"
#include "script/standard.h"
#include "stat.h"
#include "timedata.h"
#include "tinyformat.h"
#include "tweak.h"
#include "txadmission.h"
#include "txmempool.h"
#include "txorphanpool.h"
#include "ui_interface.h"
#include "unlimited.h"
#include "util.h"
#include "utilmoneystr.h"
#include "utilstrencodings.h"
#include "validation/validation.h"
#include "validationinterface.h"
#include "version.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#include "wallet/walletdb.h"
#endif

#include <atomic>
#include <boost/lexical_cast.hpp>
#include <inttypes.h>
#include <iomanip>
#include <limits>
#include <queue>
#include <stack>
#include <thread>
// Execute a command, as given by -alertnotify, on certain events such as a long fork being seen
extern void AlertNotify(const std::string &strMessage);

using namespace std;

extern CTxMemPool mempool; // from main.cpp
static atomic<bool> fIsChainNearlySyncd{false};

// We always start with true so that when ActivateBestChain is called during the startup (init.cpp)
// and we havn't finished initial sync then we don't accidentally trigger the auto-dbcache
// resize. After ActivateBestChain the fIsInitialBlockDownload flag is set to true or false depending
// on whether we really have finished sync or not.
static std::atomic<bool> fIsInitialBlockDownload{true};

int64_t nMaxTipAge = DEFAULT_MAX_TIP_AGE;

bool IsTrafficShapingEnabled();
UniValue validateblocktemplate(const UniValue &params, bool fHelp);
UniValue validatechainhistory(const UniValue &params, bool fHelp);
UniValue issuealert(const UniValue &params, bool fHelp);

void makeLowercase(std::string &input)
{
    for (auto &c : input)
        c = ::tolower(c);
}

std::string OutboundConnectionValidator(const int &value, int *item, bool validate)
{
    if (validate)
    {
        if ((value < 0) || (value > 10000)) // sanity check
        {
            return "Invalid Value";
        }
        if (value < *item)
        {
            return "This field cannot be reduced at run time since that would kick out existing connections";
        }
    }
    else // Do anything to "take" the new value
    {
        if (value < *item) // note that now value is the old value and *item has been set to the new.
        {
            int diff = *item - value;
            if (semOutboundAddNode) // Add the additional slots to the outbound semaphore
                for (int i = 0; i < diff; i++)
                    semOutboundAddNode->post();
        }
    }
    return std::string();
}

std::string SubverValidator(const std::string &value, std::string *item, bool validate)
{
    if (validate)
    {
        if (value.size() > MAX_SUBVERSION_LENGTH)
        {
            return (std::string("Subversion string is too long"));
        }
    }
    return std::string();
}

std::string Bip135VoteValidator(const std::string &value, std::string *item, bool validate)
{
    if (validate)
    {
        bool categoriesValid = AssignBip135Votes(value, -1);
        if (!categoriesValid)
            return std::string("Invalid/unknown features specified");
    }
    else // Do what is needed to use the new value already stored in item
    {
        ClearBip135Votes();
        AssignBip135Votes(*item, 1);
        SignalBlockTemplateChange();
    }
    return std::string();
}

// Ensure that only one fork can be active at a time, and convert values of 1 to the
// fork time default.
std::string ForkTimeValidator(const uint64_t &value, uint64_t *item, bool validate)
{
    if (validate)
    {
    }
    else // If it was just turned "on" then set to the default activation time.
    {
        if (*item == 1)
        {
            *item = Params().GetConsensus().nextForkActivationTime;
        }
    }
    return std::string();
}

// Push all transactions in the mempool to another node
void UnlimitedPushTxns(CNode *dest);

void UpdateSendStats(CNode *pfrom, const char *strCommand, int msgSize, int64_t nTime)
{
    sendAmt += msgSize;
    std::string name("net/send/msg/");
    name.append(strCommand);
    LOCK(cs_statMap);
    CStatMap::iterator obj = statistics.find(name);
    CStatMap::iterator end = statistics.end();
    if (obj != end)
    {
        CStatBase *base = obj->second;
        if (base)
        {
            CStatHistory<uint64_t> *stat = dynamic_cast<CStatHistory<uint64_t> *>(base);
            if (stat)
                *stat << msgSize;
        }
    }
}

void UpdateRecvStats(CNode *pfrom, const std::string &strCommand, int msgSize, int64_t nStopwatchTimeReceived)
{
    recvAmt += msgSize;
    std::string name = "net/recv/msg/" + strCommand;
    LOCK(cs_statMap);
    CStatMap::iterator obj = statistics.find(name);
    CStatMap::iterator end = statistics.end();
    if (obj != end)
    {
        CStatBase *base = obj->second;
        if (base)
        {
            CStatHistory<uint64_t> *stat = dynamic_cast<CStatHistory<uint64_t> *>(base);
            if (stat)
                *stat << msgSize;
        }
    }
}


std::string FormatCoinbaseMessage(const std::vector<std::string> &comments, const std::string &customComment)
{
    std::ostringstream ss;
    if (!comments.empty())
    {
        std::vector<std::string>::const_iterator it(comments.begin());
        ss << "/" << *it;
        for (++it; it != comments.end(); ++it)
            ss << "/" << *it;
        ss << "/";
    }
    std::string ret = ss.str() + customComment;
    return ret;
}

CNodeRef FindLikelyNode(const std::string &addrName)
{
    LOCK(cs_vNodes);
    // always match any beginning part of string to be
    // compatible with old implementation of FindLikelyNode(..)
    std::string match_str = (addrName[addrName.size() - 1] == '*') ? addrName : addrName + "*";
    for (CNode *pnode : vNodes)
    {
        if (wildmatch(match_str, pnode->addrName))
            return pnode;
    }
    return nullptr;
}

UniValue expedited(const UniValue &params, bool fHelp)
{
    if (fHelp || params.size() < 2)
        throw runtime_error("expedited block|tx \"node IP addr\" on|off\n"
                            "\nRequest expedited forwarding of blocks and/or transactions from a node.\nExpedited "
                            "forwarding sends blocks or transactions to a node before the node requests them.  This "
                            "reduces latency, potentially at the expense of bandwidth.\n"
                            "\nArguments:\n"
                            "1. \"block | tx\"        (string, required) choose block to send expedited blocks, tx to "
                            "send expedited transactions\n"
                            "2. \"node ip addr\"     (string, required) The node's IP address or IP and port (see "
                            "getpeerinfo for nodes)\n"
                            "3. \"on | off\"     (string, required) Turn expedited service on or off\n"
                            "\nExamples:\n" +
                            HelpExampleCli("expedited", "block \"192.168.0.6:8333\" on") +
                            HelpExampleRpc("expedited", "\"block\", \"192.168.0.6:8333\", \"on\""));

    std::string obj = params[0].get_str();
    std::string strNode = params[1].get_str();

    CNodeRef node(FindLikelyNode(strNode));
    if (!node)
    {
        throw runtime_error("Unknown node");
    }

    uint64_t flags = 0;
    if (obj.find("block") != std::string::npos)
        flags |= EXPEDITED_BLOCKS;
    if (obj.find("blk") != std::string::npos)
        flags |= EXPEDITED_BLOCKS;
    if (obj.find("tx") != std::string::npos)
        flags |= EXPEDITED_TXNS;
    if (obj.find("transaction") != std::string::npos)
        flags |= EXPEDITED_TXNS;
    if ((flags & (EXPEDITED_BLOCKS | EXPEDITED_TXNS)) == 0)
    {
        throw runtime_error("Unknown object, give 'block' or 'transaction'");
    }

    if (params.size() >= 3)
    {
        std::string onoff = params[2].get_str();
        if (onoff == "off")
            flags |= EXPEDITED_STOP;
        if (onoff == "OFF")
            flags |= EXPEDITED_STOP;
    }

    connmgr->PushExpeditedRequest(node.get(), flags);

    return NullUniValue;
}

UniValue pushtx(const UniValue &params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error("pushtx \"node\"\n"
                            "\nPush uncommitted transactions to a node.\n"
                            "\nArguments:\n"
                            "1. \"node\"     (string, required) The node (see getpeerinfo for nodes)\n"
                            "\nExamples:\n" +
                            HelpExampleCli("pushtx", "\"192.168.0.6:8333\" ") +
                            HelpExampleRpc("pushtx", "\"192.168.0.6:8333\", "));

    string strNode = params[0].get_str();

    CNodeRef node(FindLikelyNode(strNode));
    if (!node)
        throw runtime_error("Unknown node");

    UnlimitedPushTxns(node.get());

    return NullUniValue;
}

void UnlimitedPushTxns(CNode *dest)
{
    std::vector<uint256> vtxid;
    mempool.queryIds(vtxid);
    vector<CInv> vInv;
    unsigned int count = 0;
    for (uint256 &hash : vtxid)
    {
        // Periodically check to make sure we are not shutting down.  It can take a long time to push all txns
        // when mempools are huge.
        count++;
        if (((count & 0xFFF) == 0) && ShutdownRequested())
            return; // abort because shutting down

        CInv inv(MSG_TX, hash);
        CTransactionRef ptx = nullptr;
        ptx = mempool.get(hash);
        if (ptx == nullptr)
            continue; // another thread removed since queryHashes

        // Dont hold this lock during a PushMessage.  This code isn't really perf sensitive so ok to lock in the loop
        {
            LOCK(dest->cs_filter);
            if ((dest->pfilter && dest->pfilter->IsRelevantAndUpdate(ptx)) || (!dest->pfilter))
                vInv.push_back(inv);
        }

        if (vInv.size() == MAX_INV_SZ)
        {
            dest->PushMessage("inv", vInv);
            vInv.clear();
        }
    }
    if (vInv.size() > 0)
        dest->PushMessage("inv", vInv);
}

void UnlimitedSetup(void)
{
    blockVersion = GetArg("-blockversion", blockVersion);
    LoadTweaks(); // The above options are deprecated so the same parameter defined as a tweak will override them

    // If the user configures it to 1, assume this means default
    if (miningForkTime.Value() == 1)
        miningForkTime = Params().GetConsensus().nextForkActivationTime;

    //  Init network shapers
    int64_t rb = GetArg("-receiveburst", DEFAULT_MAX_RECV_BURST);
    // parameter is in KBytes/sec, leaky bucket is in bytes/sec.  But if it is "off" then don't multiply
    if (rb != std::numeric_limits<long long>::max())
        rb *= 1024;
    int64_t ra = GetArg("-receiveavg", DEFAULT_AVE_RECV);
    if (ra != std::numeric_limits<long long>::max())
        ra *= 1024;
    int64_t sb = GetArg("-sendburst", DEFAULT_MAX_SEND_BURST);
    if (sb != std::numeric_limits<long long>::max())
        sb *= 1024;
    int64_t sa = GetArg("-sendavg", DEFAULT_AVE_SEND);
    if (sa != std::numeric_limits<long long>::max())
        sa *= 1024;

    receiveShaper.set(rb, ra);
    sendShaper.set(sb, sa);

    txAdded.init("txpool/txAdded");
    poolSize.init("txpool/size", STAT_OP_AVE | STAT_KEEP);
    recvAmt.init("net/recv/total");
    recvAmt.init("net/send/total");
    std::vector<std::string> msgTypes = getAllNetMessageTypes();

    for (std::vector<std::string>::const_iterator i = msgTypes.begin(); i != msgTypes.end(); ++i)
    {
        mallocedStats.push_front(new CStatHistory<uint64_t>("net/recv/msg/" + *i));
        mallocedStats.push_front(new CStatHistory<uint64_t>("net/send/msg/" + *i));
    }
}

FILE *blockReceiptLog = nullptr;

void UnlimitedCleanup()
{
    txAdded.Stop();
    poolSize.Stop();
    recvAmt.Stop();
    sendAmt.Stop();
    nTxValidationTime.Stop();
    {
        LOCK(cs_blockvalidationtime);
        nBlockValidationTime.Stop();
    }

    CStatBase *obj = nullptr;
    while (!mallocedStats.empty())
    {
        obj = mallocedStats.front();
        delete obj;
        mallocedStats.pop_front();
    }
}

extern void UnlimitedLogBlock(const CBlock &block, const std::string &hash, uint64_t receiptTime)
{
#if 0 // remove block logging for official release
    if (!blockReceiptLog)
        blockReceiptLog = fopen("blockReceiptLog.txt", "a");
    if (blockReceiptLog) {
        long int byteLen = block.GetBlockSize();
        CBlockHeader bh = block.GetBlockHeader();
        fprintf(blockReceiptLog, "%" PRIu64 ",%" PRIu64 ",%ld,%ld,%s\n", receiptTime, (uint64_t)bh.nTime, byteLen, block.vtx.size(), hash.c_str());
        fflush(blockReceiptLog);
    }
#endif
}


std::string LicenseInfo()
{
    return FormatParagraph(strprintf(_("Copyright (C) 2015-%i The Bitcoin Unlimited Developers"), COPYRIGHT_YEAR)) +
           "\n\n" +
           FormatParagraph(strprintf(_("Portions Copyright (C) 2009-%i The Bitcoin Core Developers"), COPYRIGHT_YEAR)) +
           "\n\n" +
           FormatParagraph(strprintf(_("Portions Copyright (C) 2014-%i The Bitcoin XT Developers"), COPYRIGHT_YEAR)) +
           "\n\n" + "\n" + FormatParagraph(_("This is experimental software.")) + "\n" + "\n" +
           FormatParagraph(_("Distributed under the MIT software license, see the accompanying file COPYING or "
                             "<http://www.opensource.org/licenses/mit-license.php>.")) +
           "\n" + "\n" +
           FormatParagraph(_("This product includes software developed by the OpenSSL Project for use in "
                             "the OpenSSL Toolkit <https://www.openssl.org/> and cryptographic software "
                             "written by Eric Young and UPnP software written by Thomas Bernard.")) +
           "\n";
}

extern UniValue getminercomment(const UniValue &params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error("getminercomment\n"
                            "\nReturn the comment that will be put into each mined block's coinbase\n transaction "
                            "after the standard parameters."
                            "\nResult\n"
                            "  minerComment (string) miner comment\n"
                            "\nExamples:\n" +
                            HelpExampleCli("getminercomment", "") + HelpExampleRpc("getminercomment", ""));

    return minerComment;
}

extern UniValue setminercomment(const UniValue &params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error("setminercomment\n"
                            "\nSet the comment that will be put into each mined block's coinbase\n transaction after "
                            "the standard parameters.\n Comments that are too long will be truncated."
                            "\nExamples:\n" +
                            HelpExampleCli("setminercomment", "\"nakamoto consensus is emergent\"") +
                            HelpExampleRpc("setminercomment", "\"nakamoto consensus is emergent\""));

    minerComment = params[0].getValStr();
    return NullUniValue;
}

UniValue getminingmaxblock(const UniValue &params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error("getminingmaxblock\n"
                            "\nReturn the max generated (mined) block size"
                            "\nResult\n"
                            "      (integer) maximum generated block size in bytes\n"
                            "\nExamples:\n" +
                            HelpExampleCli("getminingmaxblock", "") + HelpExampleRpc("getminingmaxblock", ""));

    uint64_t nBlockMaxSize = miningBlockSize.Value();
    if (nBlockMaxSize == 0)
        nBlockMaxSize = chainActive.Tip()->GetNextMaxBlockSize();
    if (nBlockMaxSize > DEFAULT_LARGEST_BLOCKSIZE_POSSIBLE)
        nBlockMaxSize = DEFAULT_LARGEST_BLOCKSIZE_POSSIBLE;

    return nBlockMaxSize;
}


UniValue setminingmaxblock(const UniValue &params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "setminingmaxblock blocksize\n"
            "\nSet the maximum number of bytes to include in a generated (mined) block.  This command does not turn "
            "generation on/off.\n"
            "\nArguments:\n"
            "1. blocksize         (integer, required) the maximum number of bytes to include in a block.\n"
            "\nExamples:\n"
            "\nSet the generated block size limit to 8 MB\n" +
            HelpExampleCli("setminingmaxblock", "8000000") + "\nCheck the setting\n" +
            HelpExampleCli("getminingmaxblock", ""));

    uint64_t arg = 0;
    if (params[0].isNum())
        arg = params[0].get_int64();
    else
    {
        string temp = params[0].get_str();
        if (temp[0] == '-')
            boost::throw_exception(boost::bad_lexical_cast());
        arg = boost::lexical_cast<uint64_t>(temp);
    }

    // I don't want to waste time testing edge conditions where no txns can fit in a block, so limit the minimum block
    // size
    // This also fixes issues user issues where people provide the value as MB
    if (arg < 100)
        throw runtime_error("max generated block size must be greater than 100 bytes");

    std::string ret = miningBlockSize.Validate(params[0]);
    if (!ret.empty())
        throw runtime_error(ret.c_str());
    return miningBlockSize.Set(params[0]);
}

UniValue getblockversion(const UniValue &params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error("getblockversion\n"
                            "\nReturn the block version used when mining."
                            "\nResult\n"
                            "      (integer) block version number\n"
                            "\nExamples:\n" +
                            HelpExampleCli("getblockversion", "") + HelpExampleRpc("getblockversion", ""));
    const CBlockIndex *pindex = chainActive.Tip();
    return UnlimitedComputeBlockVersion(pindex, Params().GetConsensus(), pindex->time());
}

UniValue setblockversion(const UniValue &params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error("setblockversion blockVersionNumber\n"
                            "\nSet the block version number.\n"
                            "\nArguments:\n"
                            "1. blockVersionNumber         (integer, hex integer, 'BIP109', 'BASE' or 'default'.  "
                            "Required) The block version number.\n"
                            "\nExamples:\n"
                            "\nVote for 2MB blocks\n" +
                            HelpExampleCli("setblockversion", "BIP109") + "\nCheck the setting\n" +
                            HelpExampleCli("getblockversion", ""));

    uint32_t arg = 0;

    string temp = params[0].get_str();
    if (temp == "default")
    {
        arg = 0;
    }
    else if (temp == "BIP109")
    {
        arg = BASE_VERSION | FORK_BIT_2MB;
    }
    else if (temp == "BASE")
    {
        arg = BASE_VERSION;
    }
    else if ((temp[0] == '0') && (temp[1] == 'x'))
    {
        std::stringstream ss;
        ss << std::hex << (temp.c_str() + 2);
        ss >> arg;
    }
    else
    {
        arg = boost::lexical_cast<unsigned int>(temp);
    }

    blockVersion = arg;

    return NullUniValue;
}

bool IsTrafficShapingEnabled()
{
    int64_t max, avg;

    sendShaper.get(&max, &avg);
    if (avg != std::numeric_limits<long long>::max() || max != std::numeric_limits<long long>::max())
        return true;

    receiveShaper.get(&max, &avg);
    if (avg != std::numeric_limits<long long>::max() || max != std::numeric_limits<long long>::max())
        return true;

    return false;
}

UniValue gettrafficshaping(const UniValue &params, bool fHelp)
{
    if (fHelp || (params.size() != 0))
        throw runtime_error(
            "gettrafficshaping"
            "\nReturns the current settings for the network send and receive bandwidth and burst in kilobytes per "
            "second.\n"
            "\nArguments: None\n"
            "\nResult:\n"
            "  {\n"
            "    \"sendBurst\" : 40,   (string) The maximum send bandwidth in Kbytes/sec\n"
            "    \"sendAve\" : 30,   (string) The average send bandwidth in Kbytes/sec\n"
            "    \"recvBurst\" : 20,   (string) The maximum receive bandwidth in Kbytes/sec\n"
            "    \"recvAve\" : 10,   (string) The average receive bandwidth in Kbytes/sec\n"
            "  }\n"
            "\n NOTE: if the send and/or recv parameters do not exist, shaping in that direction is disabled.\n"
            "\nExamples:\n" +
            HelpExampleCli("gettrafficshaping", "") + HelpExampleRpc("gettrafficshaping", ""));

    UniValue ret(UniValue::VOBJ);
    int64_t max, avg;
    sendShaper.get(&max, &avg);
    if (avg != std::numeric_limits<long long>::max() || max != std::numeric_limits<long long>::max())
    {
        ret.pushKV("sendBurst", max / 1024);
        ret.pushKV("sendAve", avg / 1024);
    }
    receiveShaper.get(&max, &avg);
    if (avg != std::numeric_limits<long long>::max() || max != std::numeric_limits<long long>::max())
    {
        ret.pushKV("recvBurst", max / 1024);
        ret.pushKV("recvAve", avg / 1024);
    }
    return ret;
}

UniValue settrafficshaping(const UniValue &params, bool fHelp)
{
    bool disable = false;
    bool badArg = false;
    CLeakyBucket *bucket = nullptr;
    if (params.size() >= 2)
    {
        const string strCommand = params[0].get_str();
        if (strCommand == "send")
            bucket = &sendShaper;
        if (strCommand == "receive")
            bucket = &receiveShaper;
        if (strCommand == "recv")
            bucket = &receiveShaper;
    }
    if (params.size() == 2)
    {
        if (params[1].get_str() == "disable")
            disable = true;
        else
            badArg = true;
    }
    else if (params.size() != 3)
        badArg = true;

    if (fHelp || badArg || bucket == nullptr)
        throw runtime_error(
            "settrafficshaping \"send|receive\" \"burstKB\" \"averageKB\""
            "\nSets the network send or receive bandwidth and burst in kilobytes per second.\n"
            "\nArguments:\n"
            "1. \"send|receive\"     (string, required) Are you setting the transmit or receive bandwidth\n"
            "2. \"burst\"  (integer, required) Specify the maximum burst size in Kbytes/sec (actual max will be 1 "
            "packet larger than this number)\n"
            "2. \"average\"  (integer, required) Specify the average throughput in Kbytes/sec\n"
            "\nExamples:\n" +
            HelpExampleCli("settrafficshaping", "\"receive\" 10000 1024") +
            HelpExampleCli("settrafficshaping", "\"receive\" disable") +
            HelpExampleRpc("settrafficshaping", "\"receive\" 10000 1024"));

    if (disable)
    {
        if (bucket)
            bucket->disable();
    }
    else
    {
        uint64_t burst;
        uint64_t ave;
        if (params[1].isNum())
            burst = params[1].get_int64();
        else
        {
            string temp = params[1].get_str();
            burst = boost::lexical_cast<uint64_t>(temp);
        }
        if (params[2].isNum())
            ave = params[2].get_int64();
        else
        {
            string temp = params[2].get_str();
            ave = boost::lexical_cast<uint64_t>(temp);
        }
        if (burst < ave)
        {
            throw runtime_error("Burst rate must be greater than the average rate"
                                "\nsettrafficshaping \"send|receive\" \"burst\" \"average\"");
        }

        bucket->set(burst * 1024, ave * 1024);
    }

    return NullUniValue;
}

// fIsInitialBlockDownload is updated only during startup and whenever we receive a header.
// This way we avoid having to lock cs_main so often which tends to be a bottleneck.
void IsInitialBlockDownloadInit(bool *fInit)
{
    // For unit testing purposes, this step allows us to explicitly set the state of block sync.
    if (fInit)
    {
        fIsInitialBlockDownload.store(*fInit);
        return;
    }

    const CChainParams &chainParams = Params();
    LOCK(cs_main);
    if (!pindexBestHeader.load())
    {
        // Not nearly synced if we don't have any blocks!
        fIsInitialBlockDownload.store(true);
        return;
    }
    if (fImporting || fReindex)
    {
        fIsInitialBlockDownload.store(true);
        return;
    }
    if (fCheckpointsEnabled && chainActive.Height() < Checkpoints::GetTotalBlocksEstimate(chainParams.Checkpoints()))
    {
        fIsInitialBlockDownload.store(true);
        return;
    }

    // Using fInitialSyncComplete, once the chain is caught up the first time, and if we fall behind again due to a
    // large re-org or for lack of mined blocks, then we continue to return false for IsInitialBlockDownload().
    static bool fInitialSyncComplete = false;
    if (fInitialSyncComplete)
    {
        fIsInitialBlockDownload.store(false);
        return;
    }

    bool state = (chainActive.Height() < pindexBestHeader.load()->height() - 24 * 6 ||
                  std::max(chainActive.Tip()->GetBlockTime(), pindexBestHeader.load()->GetBlockTime()) <
                      GetTime() - nMaxTipAge);
    if (!state)
        fInitialSyncComplete = true;
    fIsInitialBlockDownload.store(state);
    return;
}

bool IsInitialBlockDownload() { return fIsInitialBlockDownload.load(); }
// fIsChainNearlySyncd is updated only during startup and whenever we receive a header.
// This way we avoid having to lock cs_main so often which tends to be a bottleneck.
void IsChainNearlySyncdInit()
{
    LOCK(cs_main);
    if (!pindexBestHeader.load())
    {
        // Not nearly synced if we don't have any blocks!
        fIsChainNearlySyncd.store(false);
    }
    else
    {
        if (chainActive.Height() < pindexBestHeader.load()->height() - DEFAULT_BLOCKS_FROM_TIP)
            fIsChainNearlySyncd.store(false);
        else
            fIsChainNearlySyncd.store(true);
    }
}

bool IsChainNearlySyncd() { return fIsChainNearlySyncd.load(); }
// Used for unit tests to artificially set the state of chain sync
void IsChainNearlySyncdSet(bool fSync) { fIsChainNearlySyncd.store(fSync); }
bool IsChainSyncd()
{
    // lock free since both are atomics
    return pindexBestHeader.load() == chainActive.Tip();
}

/** Returns the block height of the current active chain tip. **/
int GetBlockchainHeight()
{
    LOCK(cs_main);
    return chainActive.Height();
}

void LoadFilter(CNode *pfrom, CBloomFilter *filter)
{
    if (!filter->IsWithinSizeConstraints())
        // There is no excuse for sending a too-large filter
        dosMan.Misbehaving(pfrom, 100);
    else
    {
        uint64_t nSizeFilter;
        {
            LOCK(pfrom->cs_filter);
            nSizeFilter = ::GetSerializeSize(*pfrom->pThinBlockFilter, SER_NETWORK, PROTOCOL_VERSION);
            thindata.UpdateInBoundBloomFilter(nSizeFilter);
            delete pfrom->pThinBlockFilter;
            pfrom->pThinBlockFilter = new CBloomFilter(*filter);
        }
        LOG(THIN, "Thinblock Bloom filter size: %d\n", nSizeFilter);
    }
}

// Statistics:

CStatBase *FindStatistic(const char *name)
{
    LOCK(cs_statMap);
    CStatMap::iterator item = statistics.find(name);
    if (item != statistics.end())
        return item->second;
    return nullptr;
}

UniValue getstatlist(const UniValue &params, bool fHelp)
{
    if (fHelp || (params.size() != 0))
        throw runtime_error("getstatlist"
                            "\nReturns a list of all statistics available on this node.\n"
                            "\nArguments: None\n"
                            "\nResult:\n"
                            "  {\n"
                            "    \"name\" : (string) name of the statistic\n"
                            "    ...\n"
                            "  }\n"
                            "\nExamples:\n" +
                            HelpExampleCli("getstatlist", "") + HelpExampleRpc("getstatlist", ""));

    CStatMap::iterator it;

    UniValue ret(UniValue::VARR);
    LOCK(cs_statMap);
    for (it = statistics.begin(); it != statistics.end(); ++it)
    {
        ret.push_back(it->first);
    }

    return ret;
}

UniValue getstat(const UniValue &params, bool fHelp)
{
    string specificIssue;
    bool verbose = false;

    // check for param  --verbose or -v
    string::size_type params_offset = 0;
    if (params[0].isStr() && (params[0].get_str() == "--verbose" || params[0].get_str() == "-v"))
    {
        verbose = true;
        ++params_offset;
    }

    int count = 0;
    if (params.size() < (3 + params_offset))
        count = 1; // if a count is not specified, give the latest sample
    else
    {
        if (!params[2 + params_offset].isNum())
        {
            try
            {
                count = boost::lexical_cast<int>(params[2 + params_offset].get_str());
            }
            catch (const boost::bad_lexical_cast &)
            {
                fHelp = true;
                specificIssue = "Invalid argument 3 \"count\" -- not a number";
            }
        }
        else
        {
            count = params[2 + params_offset].get_int();
        }
    }
    if (fHelp || (params.size() < (1 + params_offset)))
        throw runtime_error("getstat"
                            "\nReturns the current settings for the network send and receive bandwidth and burst in "
                            "kilobytes per second.\nTo get a list of available statistics use \"getstatlist\".\n"
                            "\nArguments: \n"
                            "1. \"-v\" or \"--verbose\" (string, optional) full details\n"
                            "2. \"statistic\"     (string, required) Specify what statistic you want\n"
                            "3. \"series\"  (string, optional) Specify what data series you want.  Options are "
                            "\"total\", \"now\",\"all\", \"sec10\", \"min5\", \"hourly\", \"daily\",\"monthly\".  "
                            "Default is all.\n"
                            "4. \"count\"  (string, optional) Specify the number of samples you want.\n"

                            "\nResult:\n"
                            "  {\n"
                            "    \"<statistic name>\"\n"
                            "    {\n"
                            "    \"<series meta>\"\n (Only with --verbose|-v) "
                            "      [\n"
                            "        \"Series\": Requested series.\n"
                            "        \"SampleSize\": Requested sample group size.\"\n"
                            "      ],\n"
                            "    \"<series name>\"\n"
                            "      [\n"
                            "      <data>, (any type) The data points in the series\n"
                            "      ],\n"
                            "    \"timestamp\"\n"
                            "      [\n"
                            "      <time> (time only with --verbose|-v)\n"
                            "      ],\n"
                            "    ...\n"
                            "    },\n"
                            "  ...\n"
                            "  }\n"
                            "\nExamples:\n" +
                            HelpExampleCli("getstat", "") + HelpExampleRpc("getstat", "") + "\n" + specificIssue);

    UniValue ret(UniValue::VARR);

    string seriesStr;
    if (params.size() < (2 + params_offset))
        seriesStr = "total";
    else
        seriesStr = params[1 + params_offset].get_str();
    // uint_t series = 0;
    // if (series == "now") series |= 1;
    // if (series == "all") series = 0xfffffff;
    LOCK(cs_statMap);

    CStatBase *base = FindStatistic(params[0 + params_offset].get_str().c_str());
    if (base)
    {
        UniValue ustat(UniValue::VOBJ);
        if (seriesStr == "now")
        {
            ustat.pushKV("now", base->GetNow());
        }
        else if (seriesStr == "total")
        {
            ustat.pushKV("total", base->GetTotal());
        }
        else
        {
            UniValue series;
            if (verbose)
            {
                series = base->GetSeriesTime(seriesStr, count);

                string metaStr = "meta";
                UniValue metaData(UniValue::VARR);
                metaData.push_back("Series:" + seriesStr);
                metaData.push_back("SampleSize:" + boost::lexical_cast<std::string>(count));
                ustat.pushKV(metaStr, metaData);
                ustat.pushKV(seriesStr, series[0]);
                ustat.pushKV("timestamp", series[1]);
            }
            else
            {
                series = base->GetSeries(seriesStr, count);
                ustat.pushKV(seriesStr, series);
            }
        }

        ret.push_back(ustat);
    }
    if (ret.empty())
        throw std::invalid_argument("No stat available for that selection");

    return ret;
}

UniValue setlog(const UniValue &params, bool fHelp)
{
    // Uses internal log functions
    // Logging::
    // Don't use them in other places.

    UniValue ret = UniValue("");
    uint64_t catg = NONE;
    int nparm = params.size();
    bool action = false;

    if (fHelp || nparm > 2)
    {
        throw runtime_error(
            "log \"category|all\" \"on|off\""
            "\nTurn categories on or off\n"
            "\nWith no arguments it returns a list of currently on log categories\n"
            "\nArguments:\n"
            "1. \"category|all\" (string, required) Category or all categories\n"
            "2. \"on\"           (string, optional) Turn a category, or all categories, on\n"
            "2. \"off\"          (string, optional) Turn a category, or all categories, off\n"
            "2.                (string, optional) No argument. Show a category, or all categories, state: on|off\n" +
            HelpExampleCli("log", "\"NET\" on") + HelpExampleCli("log", "\"all\" off") +
            HelpExampleCli("log", "\"tor\" ") + HelpExampleCli("log", "\"ALL\" ") + HelpExampleCli("log", " "));
    }

    if (nparm == 0)
        ret = UniValue(Logging::LogGetAllString(true));

    if (nparm > 0)
    {
        std::string category;
        std::string data = params[0].get_str();
        std::transform(data.begin(), data.end(), std::back_inserter(category), ::tolower);
        catg = Logging::LogFindCategory(category);
        if (catg == NONE)
        {
            throw std::invalid_argument("Category not found: " + params[0].get_str());
        }
    }

    switch (nparm)
    {
    case 1:
        if (catg == ALL)
            ret = UniValue(Logging::LogGetAllString());
        else
            ret = UniValue(Logging::LogAcceptCategory(catg) ? "on" : "off");
        break;

    case 2:
        action = IsStringTrue(params[1].get_str());

        Logging::LogToggleCategory(catg, action);

    default:
        break;
    }

    return ret;
}

/** Mining-Candidate begin */
static int64_t lastMiningCandidateId = 0;

/** Oustanding candidates are removed 30 sec after a new block has been found*/
static void RmOldMiningCandidates()
{
    LOCK(csMiningCandidates);
    unsigned int height = GetBlockchainHeight();

    // Clean out mining candidates that are the same height as a discovered block.
    for (auto it = miningCandidatesMap.cbegin(); it != miningCandidatesMap.cend();)
    {
        if ((it->second.block == nullptr) || (it->second.block->GetHeight() <= height))
        {
            it = miningCandidatesMap.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // LOGA("Mining Candidate Map Size: %d\n", miningCandidatesMap.size());
    auto maxCandid = maxMiningCandidates.Value();
    if (maxCandid <= 0)
        maxCandid = 1; // sanity check
    if (miningCandidatesMap.size() > maxCandid)
    {
        miningCandidatesMap.erase(lastMiningCandidateId - (maxCandid - 1));
        // LOGA("New Mining Candidate Map Size: %d\n", miningCandidatesMap.size());
    }
}


static uint64_t AddMiningCandidate(CMiningCandidate &candid)
{
    // Save candidate so can be looked up:
    LOCK(csMiningCandidates);
    lastMiningCandidateId++;
    candid.id = lastMiningCandidateId;
    miningCandidatesMap[lastMiningCandidateId] = candid;
    return lastMiningCandidateId;
}

std::vector<uint256> GetMerkleProofBranches(CBlock *pblock)
{
    std::vector<uint256> ret;
    std::vector<uint256> leaves;
    int len = pblock->vtx.size();

    for (int i = 0; i < len; i++)
    {
        leaves.push_back(pblock->vtx[i].get()->GetId());
    }

    ret = ComputeMerkleBranch(leaves, 0);
    return ret;
}

static CMiningCandidate *FindRecentMiningCandidate(CScript *coinbaseScript)
{
    LOCK(csMiningCandidates);
    if ((lastMiningCandidateId == 0) || (miningCandidatesMap.size() == 0))
        return nullptr; // No candidates
    auto it = miningCandidatesMap.find(lastMiningCandidateId);
    if (it == miningCandidatesMap.end())
        return nullptr; // latest candidate has been removed
    CMiningCandidate &candid = it->second;
    if (candid.creationTime + minMiningCandidateInterval.Value() <= (uint64_t)GetTime())
        return nullptr; // Too old

    // I don't care what the coinbase script is (probably because its anything from this wallet)
    if (coinbaseScript == nullptr)
        return &candid;

    // The last candidate's script was the same as what I want now.
    if (*coinbaseScript == candid.block->vtx[0]->vout[0].scriptPubKey)
        return &candid;

    return nullptr;
}

/** Create Mining-Candidate JSON to send to miner */
static UniValue MkMiningCandidateJson(CMiningCandidate &candid)
{
    UniValue ret(UniValue::VOBJ);
    CBlock &block = *(candid.block);

    ret.pushKV("id", candid.id);
    ret.pushKV("headerCommitment", block.GetMiningHeaderCommitment().GetHex());
    ret.pushKV("nBits", strprintf("%08x", block.nBits));

#if 0 // Merkle path is no longer needed for mining.  Mining pool should provide us with its output script and we'll
      // make the coinbase tx.  However, leave this code for demonstration purposes.
    ret.pushKV("prevhash", block.hashPrevBlock.GetHex());

    {
        const CTransaction *tran = block.vtx[0].get();
        ret.pushKV("coinbase", EncodeHexTx(*tran));
    }

    ret.pushKV("time", block.GetBlockTime());

    // merkleProof:
    {
        std::vector<uint256> brancharr = GetMerkleProofBranches(&block);
        UniValue merkleProof(UniValue::VARR);
        for (const auto &i : brancharr)
        {
            merkleProof.push_back(i.GetHex());
        }
        ret.pushKV("merkleProof", merkleProof);

        // merklePath parameter:
        // If the coinbase is ever allowed to be anywhere in the hash tree via a hard fork, we will need to communicate
        // how to calculate the merkleProof by supplying a bit for every level in the proof.
        // This bit tells the calculator whether the next hash is on the left or right side of the tree.
        // In other words, whether to do cat(A,B) or cat(B,A).  Specifically, if the bit is 0,the proof calcuation uses
        // Hash256(concatentate(running hash, next hash in proof)), if the bit is 1, the proof calculates
        // Hash256(concatentate(next hash in proof, running hash))

        // ret.pushKV("merklePath", 0);
    }
#endif

    return ret;
}


/** RPC Get a block candidate*/
UniValue getminingcandidate(const UniValue &params, bool fHelp)
{
    UniValue ret(UniValue::VOBJ);
    CMiningCandidate candid;
    int64_t coinbaseSize = -1; // If -1 then not used to set coinbase size

    if (fHelp || params.size() > 2)
    {
        throw runtime_error(
            "getminingcandidate"
            "\nReturns Mining-Candidate protocol data.\n"
            "\nArguments:\n"
            "1. \"coinbasesize\" (int, optional) Get a fixed size coinbase transaction.\n"
            "                                  Default: null (null indicates unspecified / use daemon defaults)\n"
            "2. \"address\"      (string, optional) The address to send the newly generated coins to.\n"
            "                                     Default: an address in daemon's wallet.\n" +
            HelpExampleCli("getminingcandidate", "") + HelpExampleCli("getminingcandidate", "1000") +
            HelpExampleCli("getminingcandidate", "1000 nexa:qq9rw090p2eu9drv6ptztwx4ghpftwfa0gyqvlvx2q") +
            HelpExampleCli("getminingcandidate", "null nexa:qq9rw090p2eu9drv6ptztwx4ghpftwfa0gyqvlvx2q"));
    }

    if (!unsafeGetBlockTemplate.Value())
    {
        {
            LOCK(cs_vNodes);
            if (vNodes.empty())
                throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "full node is not connected to the larger network!");
        }

        if (IsInitialBlockDownload())
            throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "full node is downloading blocks...");
    }

    CScript coinbaseScript;
    std::string destStr;

    // we accept: param1, param2 or just param1 by itself or null,param2 to only specify param2
    if (params.size() >= 1 && !params[0].isNull() && !params[0].getValStr().empty())
    {
        coinbaseSize = params[0].get_int64(); // may throw, which is what we want
        if (coinbaseSize < 0)
        {
            throw std::runtime_error("Requested coinbase size is less than 0");
        }

        if (coinbaseSize > ONE_MEGABYTE)
        {
            throw std::runtime_error(strprintf("Requested coinbase size too big. Max allowed: %u", ONE_MEGABYTE));
        }
    }
    if (params.size() == 2)
    {
        destStr = params[1].get_str();
    }

    RmOldMiningCandidates();

    {
        // Lock the mining candidates so that another request or a solution does not modify the coinbase while we are
        // using it
        LOCK(csMiningCandidates);
        CMiningCandidate *recentCandidate = nullptr;
        // validate destination address (if supplied)
        if (!destStr.empty())
        {
            CTxDestination destination = DecodeDestination(destStr);
            if (!IsValidDestination(destination))
            {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Error: Invalid address \"%s\"", destStr));
            }
            coinbaseScript = GetScriptForDestination(destination);
            candid.localCoinbase = false;

            // Look for a recent candidate
            recentCandidate = FindRecentMiningCandidate(&coinbaseScript);
        }
        else
        {
            candid.localCoinbase = true;

            // Look for a recent candidate
            recentCandidate = FindRecentMiningCandidate(nullptr);
        }

        if (recentCandidate)
        {
            ret = MkMiningCandidateJson(*recentCandidate);
            return ret;
        }
        else
        {
            candid.block = MakeBlockRef();
            mkblocktemplate(UniValue(UniValue::VARR), coinbaseSize, candid.block.get(), coinbaseScript);
            candid.creationTime = GetTime();

            // Save candidate so it can be looked up:
            AddMiningCandidate(candid);

            ret = MkMiningCandidateJson(candid);
            return ret;
        }
    }
}

/** RPC Submit a solved block candidate*/
UniValue submitminingsolution(const UniValue &params, bool fHelp)
{
    UniValue rcvd;
    CBlockRef block;

    if (fHelp || params.size() != 1)
    {
        throw runtime_error(
            "submitminingsolution \"Mining-Candidate data\" ( \"jsonparametersobject\" )\n"
            "\nAttempts to submit a new block to the network.\n"
            "\nArguments\n"
            "1. \"submitminingsolutiondata\"    (string, required) the mining solution (JSON encoded) data to submit\n"
            "\nResult:\n"
            "\ndictionary of 'hash', 'height' and 'result' which is empty if accepted\n"
            "and an error string if block was rejected.\n"
            "Identical to \"submitblock\".\n"
            "\nExamples:\n" +
            HelpExampleRpc("submitminingsolution", "\"mydata\""));
    }

    rcvd = params[0].get_obj();

    int64_t id = rcvd["id"].get_int64();

    {
        LOCK(csMiningCandidates);
        if (miningCandidatesMap.count(id) == 1)
        {
            block = miningCandidatesMap[id].block;
            miningCandidatesMap.erase(id);
        }
        else
        {
            return UniValue("id not found (stale candidate)");
        }
    }

    UniValue nonce = rcvd["nonce"];
    if (nonce.isNull())
    {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "nonce not found");
    }
    block->nonce = ParseHex(nonce.get_str()); // 64 bit to deal with sign bit in 32 bit unsigned int

    if (block->nonce.size() > CBlockHeader::MAX_NONCE_SIZE)
    {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "nonce too large");
    }

    UniValue uvsub = SubmitBlock(block); // returns string on failure
    RmOldMiningCandidates();
    return uvsub;
}

static void CalculateNextMerkleRoot(uint256 &merkle_root, const uint256 &merkle_branch)
{
    // Append a branch to the root. Double SHA256 the whole thing:
    uint256 hash;
    CHash256()
        .Write(merkle_root.begin(), merkle_root.size())
        .Write(merkle_branch.begin(), merkle_branch.size())
        .Finalize(hash.begin());
    merkle_root = hash;
}

uint256 CalculateMerkleRoot(uint256 &coinbase_hash, const std::vector<uint256> &merkleProof)
{
    uint256 merkle_root = coinbase_hash;
    for (unsigned int i = 0; i < merkleProof.size(); i++)
    {
        CalculateNextMerkleRoot(merkle_root, merkleProof[i]);
    }
    return merkle_root;
}

#ifdef DEBUG
// This RPC is very useful for checking that the test system works, but we don't want it to be part of the
// normal release
UniValue crash(const UniValue &params, bool fHelp)
{
    if (fHelp || params.size() >= 1)
        throw runtime_error("crash \n"
                            "\nCrashes this program, generating a core dump.\n"
                            "\nArguments\n"
                            "none\n"
                            "\nResult:\n" +
                            HelpExampleCli("crash", "") + HelpExampleRpc("crash", ""));


    abort();
}
#endif

/* clang-format off */
static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         okSafeMode
  //  --------------------- ------------------------  -----------------------  ----------
    /* P2P networking */
    { "network",            "settrafficshaping",      &settrafficshaping,      true  },
    { "network",            "gettrafficshaping",      &gettrafficshaping,      true  },
    { "network",            "pushtx",                 &pushtx,                 true  },
    { "network",            "expedited",              &expedited,              true  },

    /* Mining */
    { "mining",             "getminingmaxblock",      &getminingmaxblock,      true  },
    { "mining",             "setminingmaxblock",      &setminingmaxblock,      true  },
    { "mining",             "getminercomment",        &getminercomment,        true  },
    { "mining",             "setminercomment",        &setminercomment,        true  },
    { "mining",             "getblockversion",        &getblockversion,        true  },
    { "mining",             "setblockversion",        &setblockversion,        true  },
    { "mining",             "validateblocktemplate",  &validateblocktemplate,  true  },
    { "mining",             "getminingcandidate",     &getminingcandidate,     true  },
    { "mining",             "submitminingsolution",   &submitminingsolution,   true  },

    /* Utility functions */
    { "util",               "getstatlist",            &getstatlist,            true  },
    { "util",               "getstat",                &getstat,                true  },
    { "util",               "get",                    &gettweak,               true  },
    { "util",               "set",                    &settweak,               true  },
    { "util",               "validatechainhistory",   &validatechainhistory,   true  },
#ifdef DEBUG
    { "util",               "getstructuresizes",      &getstructuresizes,      true  },
    { "util",               "crash",                  &crash,                  true  },
#endif
    { "util",               "getaddressforms",        &getaddressforms,        true  },
    { "util",               "log",                    &setlog,                 true  },
    { "util",               "issuealert",             &issuealert,             true  },
};
/* clang-format on */

void RegisterUnlimitedRPCCommands(CRPCTable &table)
{
    for (auto cmd : commands)
        table.appendCommand(cmd);
}


UniValue validatechainhistory(const UniValue &params, bool fHelp)
{
    if (fHelp)
        throw runtime_error("validatechainhistory [hash]\n"
                            "\nUpdates a chain's valid/invalid status based on parent blocks.\n");

    std::stack<CBlockIndex *> stk;
    CBlockIndex *pos = pindexBestHeader.load();
    bool failedChain = false;
    UniValue ret = NullUniValue;

    if (params.size() >= 1)
    {
        std::string strHash = params[0].get_str();
        uint256 hash(uint256S(strHash));

        if ((pos = LookupBlockIndex(hash)) == 0)
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
    }

    LOGA("validatechainhistory starting at %d %s\n", pos->height(), pos->phashBlock->ToString());

    LOCK(cs_main); // modifying contents of CBlockIndex and setDirtyBlockIndex

    while (pos && !failedChain)
    {
        // LOGA("validate %d %s\n", pos->height(), pos->phashBlock->ToString());
        READLOCK(cs_mapBlockIndex);
        failedChain = pos->nStatus & BLOCK_FAILED_MASK;
        if (!failedChain)
        {
            stk.push(pos);
        }
        pos = pos->pprev;
    }
    if (failedChain)
    {
        ret = UniValue("Chain has a bad ancestor");
        while (!stk.empty())
        {
            pos = stk.top();
            if (pos)
            {
                WRITELOCK(cs_mapBlockIndex);
                pos->nStatus |= BLOCK_FAILED_CHILD;
            }
            setDirtyBlockIndex.insert(pos);
            stk.pop();
        }
        FlushStateToDisk();
        pindexBestHeader = FindMostWorkChain();
    }
    else
    {
        ret = UniValue("Chain is ok");
    }

    return ret;
}


UniValue validateblocktemplate(const UniValue &params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 1)
        throw runtime_error(
            "validateblocktemplate \"hexdata\"\n"
            "\nReturns whether this block template will be accepted if a hash solution is found.\n"
            "The 'jsonparametersobject' parameter is currently ignored.\n"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.\n"

            "\nArguments\n"
            "1. \"hexdata\"    (string, required) the hex-encoded block to validate (same format as submitblock)\n"
            "\nResult:\n"
            "true (boolean) submitted block template is valid\n"
            "JSONRPCException if submitted block template is invalid\n"
            "\nExamples:\n" +
            HelpExampleCli("validateblocktemplate", "\"mydata\"") +
            HelpExampleRpc("validateblocktemplate", "\"mydata\""));

    UniValue ret(UniValue::VARR);
    CBlock block;
    if (!DecodeHexBlk(block, params[0].get_str()))
    {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");
    }
    ConstCBlockRef pblock = std::make_shared<const CBlock>(block);
    CBlockIndex *pindexPrev = nullptr;

    pindexPrev = LookupBlockIndex(pblock->hashPrevBlock);
    if (!pindexPrev)
    {
        throw runtime_error("invalid block: unknown parent");
    }

    if (pindexPrev != chainActive.Tip())
    {
        throw runtime_error("invalid block: does not build on chain tip");
    }

    DbgAssert(pindexPrev, throw runtime_error("invalid block: unknown parent"));

    const CChainParams &chainparams = Params();
    CValidationState state;

    {
        LOCK(cs_main); // to freeze the state during block validity test

        if (!TestBlockValidity(state, chainparams, pblock, pindexPrev, false, true))
        {
            throw runtime_error(std::string("invalid block: ") + state.GetRejectReason());
        }
    }

    return UniValue(true);
}

#ifdef DEBUG

extern std::vector<std::string> vUseDNSSeeds;
extern std::list<CNode *> vNodesDisconnected;
extern std::set<CNetAddr> setservAddNodeAddresses;
extern std::map<uint256, CTxCommitData> *txCommitQ;
extern std::queue<CTxInputData> txDeferQ;
extern std::queue<CTxInputData> txInQ;
extern UniValue getstructuresizes(const UniValue &params, bool fHelp)
{
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("time", GetTime());
    ret.pushKV("requester.mapTxnInfo", (uint64_t)requester.mapTxnInfo.size());
    ret.pushKV("requester.mapBlkInfo", (uint64_t)requester.mapBlkInfo.size());
    unsigned long int max = 0;
    unsigned long int size = 0;
    for (CRequestManager::OdMap::iterator i = requester.mapTxnInfo.begin(); i != requester.mapTxnInfo.end(); i++)
    {
        unsigned long int temp = i->second.availableFrom.size();
        size += temp;
        if (max < temp)
            max = temp;
    }
    ret.pushKV("requester.mapTxnInfo.maxobj", (uint64_t)max);
    ret.pushKV("requester.mapTxnInfo.totobj", (uint64_t)size);

    max = 0;
    size = 0;
    for (CRequestManager::OdMap::iterator i = requester.mapBlkInfo.begin(); i != requester.mapBlkInfo.end(); i++)
    {
        unsigned long int temp = i->second.availableFrom.size();
        size += temp;
        if (max < temp)
            max = temp;
    }
    ret.pushKV("requester.mapBlkInfo.maxobj", (uint64_t)max);
    ret.pushKV("requester.mapBlkInfo.totobj", (uint64_t)size);

    ret.pushKV("mapBlockIndex", (int64_t)mapBlockIndex.size());
    // CChain
    ret.pushKV("mapLocalHost", (int64_t)mapLocalHost.size());
    ret.pushKV("CDoSManager::vWhitelistedRange", (int64_t)dosMan.vWhitelistedRange.size());
    ret.pushKV("mapInboundConnectionTracker", (int64_t)mapInboundConnectionTracker.size());
    ret.pushKV("vUseDNSSeeds", (int64_t)vUseDNSSeeds.size());
    ret.pushKV("vAddedNodes", (int64_t)vAddedNodes.size());
    ret.pushKV("setservAddNodeAddresses", (int64_t)setservAddNodeAddresses.size());
    ret.pushKV("statistics", (int64_t)statistics.size());
    ret.pushKV("tweaks", (int64_t)tweaks.size());
    ret.pushKV("mapRelay", (int64_t)mapRelay.size());
    ret.pushKV("vRelayExpiration", (int64_t)vRelayExpiration.size());

    {
        LOCK(cs_vNodes);
        ret.pushKV("vNodes", (int64_t)vNodes.size());
    }
    {
        LOCK(cs_vNodesDisconnected);
        ret.pushKV("vNodesDisconnected", (int64_t)vNodesDisconnected.size());
    }
    {
        READLOCK(orphanpool.cs_orphanpool);
        ret.pushKV("mapOrphanTransactions", (int64_t)orphanpool.mapOrphanTransactions.size());
        ret.pushKV("mapOrphanTransactionsByPrev", (int64_t)orphanpool.mapOrphanTransactionsByPrev.size());
    }
    // CAddrMan

    uint32_t nExpeditedBlocks, nExpeditedTxs, nExpeditedUpstream;
    connmgr->ExpeditedNodeCounts(nExpeditedBlocks, nExpeditedTxs, nExpeditedUpstream);
    ret.pushKV("xpeditedBlk", (uint64_t)nExpeditedBlocks);
    ret.pushKV("xpeditedBlkUp", (uint64_t)nExpeditedUpstream);
    ret.pushKV("xpeditedTxn", (uint64_t)nExpeditedTxs);

    if (txCommitQ)
        ret.pushKV("txCommitQ", (uint64_t)txCommitQ->size());
    ret.pushKV("txInQ", (uint64_t)txInQ.size());
    ret.pushKV("txDeferQ", (uint64_t)txDeferQ.size());
#ifdef DEBUG_LOCKORDER
    {
        std::lock_guard<std::mutex> lock(lockdata.dd_mutex);
        ret.pushKV("lockorders", (uint64_t)lockdata.ordertracker.size());
    }
#endif
    LOCK(cs_vNodes);
    int disconnected = 0; // watch # of disconnected nodes to ensure they are being cleaned up
    for (std::vector<CNode *>::iterator it = vNodes.begin(); it != vNodes.end(); ++it)
    {
        if (*it == nullptr)
            continue;
        CNode &inode = **it;
        UniValue node(UniValue::VOBJ);
        disconnected += (inode.fDisconnect) ? 1 : 0;

        node.pushKV("vSendMsg", (int64_t)inode.vSendMsg.size());
        node.pushKV("vRecvGetData", (int64_t)inode.vRecvGetData.size());
        node.pushKV("vRecvMsg", (int64_t)inode.vRecvMsg.size() + (int64_t)inode.vRecvMsg_handshake.size());
        {
            LOCK(inode.cs_filter);
            if (inode.pfilter)
            {
                node.pushKV("pfilter", (int64_t)::GetSerializeSize(*inode.pfilter, SER_NETWORK, PROTOCOL_VERSION));
            }
            if (inode.pThinBlockFilter)
            {
                node.pushKV("pThinBlockFilter",
                    (int64_t)::GetSerializeSize(*inode.pThinBlockFilter, SER_NETWORK, PROTOCOL_VERSION));
            }
        }

        {
            LOCK(inode.cs_vSend);
            node.pushKV("vAddrToSend", (int64_t)inode.vAddrToSend.size());
        }

        node.pushKV("vInventoryToSend", (int64_t)inode.vInventoryToSend.size());
        ret.pushKV(inode.addrName, node);
    }
    ret.pushKV("disconnectedNodes", disconnected);

    return ret;
}
#endif

/** Comparison function for sorting the getchaintips heads.  */
struct CompareBlocksByHeight
{
    bool operator()(const CBlockIndex *a, const CBlockIndex *b) const
    {
        /* Make sure that unequal blocks with the same height do not compare
           equal. Use the pointers themselves to make a distinction. */

        if (a->height() != b->height())
            return (a->height() > b->height());

        return a < b;
    }
};

void MarkAllContainingChainsInvalid(CBlockIndex *invalidBlock)
{
    LOCK(cs_main); // setDirtyBlockIndex
    READLOCK(cs_mapBlockIndex);

    bool dirty = false;
    DbgAssert(invalidBlock->nStatus & BLOCK_FAILED_MASK, return );

    // Find all the chain tips:
    std::set<CBlockIndex *, CompareBlocksByHeight> setTips;
    std::set<CBlockIndex *> setOrphans;
    std::set<CBlockIndex *> setPrevs;

    for (const PAIRTYPE(const uint256, CBlockIndex *) & item : mapBlockIndex)
    {
        if (!chainActive.Contains(item.second))
        {
            setOrphans.insert(item.second);
            setPrevs.insert(item.second->pprev);
        }
    }

    for (std::set<CBlockIndex *>::iterator it = setOrphans.begin(); it != setOrphans.end(); ++it)
    {
        if (setPrevs.erase(*it) == 0)
        {
            setTips.insert(*it);
        }
    }

    // Always report the currently active tip.
    setTips.insert(chainActive.Tip());

    for (CBlockIndex *tip : setTips)
    {
        if (tip->GetAncestor(invalidBlock->height()) == invalidBlock)
        {
            for (CBlockIndex *blk = tip; blk != invalidBlock; blk = blk->pprev)
            {
                blk->nStatus |= BLOCK_FAILED_VALID;

                if ((blk->nStatus & BLOCK_FAILED_CHILD) == 0)
                {
                    blk->nStatus |= BLOCK_FAILED_CHILD;
                    setDirtyBlockIndex.insert(blk);
                    dirty = true;
                }
            }
        }
    }

    if (dirty)
        FlushStateToDisk();
}

UniValue getaddressforms(const UniValue &params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 1)
        throw runtime_error("getaddressforms \"address\"\n"
                            "\nReturns all ways of displaying this address.\n"
                            "\nArguments\n"
                            "1. \"address\"    (string, required) the address\n"
                            "\nResult:\n"
                            "{\n"
                            "\"legacy\": \"base58 encoding (OBSOLETE DO NOT USE)\",\n"
                            "\"nexa\": \"nexa prefixed address\",\n"
                            "\nIf this wallet is unlocked and owns this address additional information is supplied:\n"
                            "\"pubkey\": \"public key corresponding to this address\"\n"
                            "\"p2pkh\": \"The pay-to-pub-key-hash address corresponding to this pubkey\"\n"
                            "\"p2pkt\": \"The pay-to-pub-key-template address corresponding to this pubkey\"\n"
                            "\nIf this address is a wrapped output script, the script is provided:\n"
                            "\"outScript\": \"The disassembly of the output script specified by this address\"\n"
                            "\"outScriptHex\": \"The hex of the output script specified by this address\"\n"
                            "}\n"
                            "\nExamples:\n" +
                            HelpExampleCli("getaddressforms", "\"address\"") +
                            HelpExampleRpc("getaddressforms", "\"address\""));

    UniValue ret(UniValue::VARR);
    CBlock block;

    CTxDestination dest = DecodeDestination(params[0].get_str());

    if (!IsValidDestination(dest))
    {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address or script");
    }

    std::string cashAddr = EncodeCashAddr(dest, Params());
    std::string legacyAddr = EncodeLegacyAddr(dest, Params());

    UniValue node(UniValue::VOBJ);
    node.pushKV("legacy", legacyAddr);
    node.pushKV("nexa", cashAddr);

#ifdef ENABLE_WALLET
    {
        LOCK(pwalletMain->cs_wallet);
        if (!pwalletMain->IsLocked())
        {
            CKey key;
            if (pwalletMain->GetKey(dest, key))
            {
                CPubKey pub = key.GetPubKey();
                node.pushKV("pubkey", pub.GetHex());
                node.pushKV("p2pkh", EncodeCashAddr(CTxDestination(pub.GetID()), Params()));
                ScriptTemplateDestination sd = P2pktOutput(pub);
                node.pushKV("p2pkt", EncodeCashAddr(sd, Params()));
            }
        }
    }
#endif

    const ScriptTemplateDestination *st = std::get_if<ScriptTemplateDestination>(&dest);
    if (st)
    {
        node.pushKV("outScript", st->toScript().GetAsm());
        node.pushKV("outScriptHex", st->toScript().GetHex());
    }
    return node;
}

UniValue issuealert(const UniValue &params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 1)
        throw runtime_error("issuealert \"alert\"\n"
                            "\ntrigger an alert (executes configured -alertnotify string).\n"
                            "\nArguments\n"
                            "1. \"alert\"    (string, required) the alert text (in quotes if in a shell)\n"
                            "\nExamples:\n" +
                            HelpExampleCli("issuealert", "\"this is an alert\"") +
                            HelpExampleRpc("issuealert", "\"this is an alert\""));

    UniValue ret(UniValue::VARR);

    AlertNotify(params[0].get_str());
    return UniValue();
}


std::string CStatusString::GetPrintable() const
{
    LOCK(cs_status_string);
    if (strSet.empty())
        return "ready";
    std::string ret;
    for (auto it : strSet)
    {
        if (!ret.empty())
            ret.append(" ");
        std::string &s = it;
        ret.append(s);
    }
    return ret;
}

void CStatusString::Set(const std::string &yourStatus)
{
    LOCK(cs_status_string);
    strSet.insert(yourStatus);
}

void CStatusString::Clear(const std::string &yourStatus)
{
    LOCK(cs_status_string);
    strSet.erase(yourStatus);
}


#include "core_io.h"
#include "utilstrencodings.h"

void dbgDumpStack(const Stack &stack)
{
    printf("stack %lu items (bottom to top): \n", stack.size());
    auto sz = stack.size();
    for (unsigned int i = 0; i < stack.size(); i++)
    {
        auto item = stack[i];
        if (item.isVch())
        {
            try
            {
                CScriptNum itemAsInt(item, false, CScriptNum::MAXIMUM_ELEMENT_SIZE_64_BIT);
                printf("%u (top%d) Num: %ld Hex: %s Script: %s\n", i, (int)i - (int)sz, itemAsInt.getint64(),
                    item.hex().c_str(), ScriptToAsmStr(CScript(item), false).c_str());
            }
            catch (scriptnum_error const &)
            {
                printf("%u (top%d) Num: NaN Hex: %s Script: %s\n", i, (int)i - (int)sz, item.hex().c_str(),
                    ScriptToAsmStr(CScript(item), false).c_str());
            }
        }
        else if (item.type == StackElementType::BIGNUM)
        {
            const BigNum &bn = item.num();
            printf("%u (top%d) BigNum: %s Hex: %s\n", i, (int)i - (int)sz, bn.str().c_str(), bn.str(16).c_str());
        }
    }
}
