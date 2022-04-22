// Copyright (c) 2018-2020 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#if defined(HAVE_CONFIG_H)
#include "bitcoin-config.h"
#endif

#include "allowed_args.h"
#include "arith_uint256.h"
#include "chainparams.h"
#include "chainparamsbase.h"
#include "consensus/params.h"
#include "fs.h"
#include "hashwrapper.h"
#include "key.h"
#include "pow.h"
#include "primitives/block.h"
#include "pubkey.h"
#include "rpc/client.h"
#include "rpc/protocol.h"
#include "streams.h"
#include "sync.h"
#include "util.h"
#include "utilstrencodings.h"

#include <cstdlib>
#include <functional>
#include <random>
#include <stdio.h>

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

#include <univalue.h>

// below two require C++11
#include <functional>
#include <random>

#ifdef DEBUG_LOCKORDER
std::atomic<bool> lockdataDestructed{false};
LockData lockdata;
#endif

// Lambda used to generate entropy, per-thread (see CpuMiner, et al below)
typedef std::function<uint32_t(void)> RandFunc;

using namespace std;

class Secp256k1Init
{
    ECCVerifyHandle globalVerifyHandle;

public:
    Secp256k1Init() { ECC_Start(); }
    ~Secp256k1Init() { ECC_Stop(); }
};

class BitcoinMinerArgs : public AllowedArgs::BitcoinCli
{
public:
    BitcoinMinerArgs(CTweakMap *pTweaks = nullptr)
    {
        addHeader(_("Mining options:"))
            .addArg("blockversion=<n>", ::AllowedArgs::requiredInt,
                _("Set the block version number. For testing only. Value must be an integer"))
            .addArg("cpus=<n>", ::AllowedArgs::requiredInt,
                _("Number of cpus to use for mining (default: 1). Value must be an integer"))
            .addArg("duration=<n>", ::AllowedArgs::requiredInt,
                _("Number of seconds to mine a particular block candidate (default: 30). Value must be an integer"))
            .addArg("nblocks=<n>", ::AllowedArgs::requiredInt,
                _("Number of blocks to mine (default: mine forever / -1). Value must be an integer"))
            .addArg("coinbasesize=<n>", ::AllowedArgs::requiredInt,
                _("Get a fixed size coinbase Tx (default: do not use / 0). Value must be an integer"))
            // requiredAmount here validates a float
            .addArg("maxdifficulty=<f>", ::AllowedArgs::requiredAmount,
                _("Set the maximum difficulty (default: no maximum) we will mine. If difficulty exceeds this value we "
                  "sleep and poll every <duration> seconds until difficulty drops below this threshold. Value must be "
                  "a float or integer"))
            .addArg("address=<string>", ::AllowedArgs::requiredStr,
                _("The address to send the newly generated bitcoin to. If omitted, will default to an address in the "
                  "bitcoin daemon's wallet."));
    }
};


/*
static CBlockHeader CpuMinerJsonToHeader(const UniValue &params)
{
    // Does not set hashMerkleRoot (Does not exist in Mining-Candidate params).
    CBlockHeader blockheader;

    // hashPrevBlock
    string tmpstr = params["prevhash"].get_str();
    std::vector<unsigned char> vec = ParseHex(tmpstr);
    std::reverse(vec.begin(), vec.end()); // sent reversed
    blockheader.hashPrevBlock = uint256(vec);

    // nTime:
    blockheader.nTime = params["time"].get_int();

    // nBits
    {
        std::stringstream ss;
        ss << std::hex << params["nBits"].get_str();
        ss >> blockheader.nBits;
    }

    return blockheader;
}
*/

static bool CpuMinerJsonToData(const UniValue &params, uint256 &headerCommitment, uint32_t &nBits)
{
    string tmpstr = params["headerCommitment"].get_str();
    std::vector<unsigned char> vec = ParseHex(tmpstr);
    std::reverse(vec.begin(), vec.end()); // sent reversed
    headerCommitment = uint256(vec);

    // nBits
    {
        std::stringstream ss;
        ss << std::hex << params["nBits"].get_str();
        ss >> nBits;
    }

    return true;
}

static bool CpuMineBlockHasherNextChain(int &ntries,
    uint256 headerCommitment,
    uint32_t nBits,
    const RandFunc &randFunc,
    const Consensus::Params &conp,
    std::vector<unsigned char> &nonce)
{
    arith_uint256 hashTarget = arith_uint256().SetCompact(nBits);
    bool found = false;

    // Note that since I have a coinbase that is unique to my hashing effort, my hashing won't duplicate a competitor's
    // efforts.  So it does not matter that we all start with few nonce bits.
    nonce.resize(4);

    unsigned int extra = randFunc();
    uint32_t startCount = randFunc();
    unsigned int count = startCount;
    while (!found)
    {
        //
        // Search
        //
        while (!found)
        {
            ++count;
            nonce[0] = count & 255;
            nonce[1] = (count >> 8) & 255;
            nonce[2] = (count >> 16) & 255;
            nonce[3] = (count >> 24) & 255;
            if (count == startCount) // Looped around, expand search space
            {
                ++extra;
                // TODO what if extra wraps around (go to 8 bytes)
                if (nonce.size() < 6)
                {
                    nonce.resize(6);
                }
                nonce[4] = extra & 255;
                nonce[5] = (extra >> 8) & 255;
            }
            uint256 miningHash = GetMiningHash(headerCommitment, nonce);
            if (CheckProofOfWork(miningHash, nBits, conp))
            {
                // Found a solution
                found = true;
                printf("proof-of-work found  \n  mining puzzle solution: %s  \ntarget: %s\n",
                    miningHash.GetHex().c_str(), hashTarget.GetHex().c_str());
                break;
            }
            if (ntries-- < 1)
            {
                return false; // Give up leave
            }
        }
    }

    return found;
}

static double GetDifficulty(uint64_t nBits)
{
    int nShift = (nBits >> 24) & 0xff;

    double dDiff = (double)0x0000ffff / (double)(nBits & 0x00ffffff);

    while (nShift < 29)
    {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29)
    {
        dDiff /= 256.0;
        nShift--;
    }
    return dDiff;
}

// trvially-constructible/copyable info for use in CpuMineBlock below to check if mining a stale block
struct BlkInfo
{
    uint64_t prevCheapHash, nBits;
};
// Thread-safe version of above for the shared variable. We do it this way
// because std::atomic<struct> isn't always available on all platforms.
class SharedBlkInfo : protected BlkInfo
{
    mutable CCriticalSection lock;

public:
    void store(const BlkInfo &o)
    {
        LOCK(lock);
        prevCheapHash = o.prevCheapHash;
        nBits = o.nBits;
    }
    bool operator==(const BlkInfo &o) const
    {
        LOCK(lock);
        return prevCheapHash == o.prevCheapHash && nBits == o.nBits;
    }
};
// shared variable: used to inform all threads when the latest block or difficulty has changed
static SharedBlkInfo sharedBlkInfo;

static UniValue CpuMineBlock(unsigned int searchDuration, const UniValue &params, bool &found, const RandFunc &randFunc)
{
    UniValue ret(UniValue::VARR);
    uint256 headerCommitment;
    const double maxdiff = GetDoubleArg("-maxdifficulty", 0.0);
    searchDuration *= 1000; // convert to millis

    found = false;

    uint32_t nBits;
    CpuMinerJsonToData(params, headerCommitment, nBits);

    // save the prev block CheapHash & current difficulty to the global shared variable right away: this will
    // potentially signal to other threads to return early if they are still mining on top of an old block (assumption
    // here is that this block is the latest result from the RPC server, which is true 99.99999% of the time.)
    const BlkInfo blkInfo = {headerCommitment.GetCheapHash(), nBits};
    sharedBlkInfo.store(blkInfo);

    // first check difficulty, and abort if it's lower than maxdifficulty from CLI
    const double difficulty = GetDifficulty(nBits);

    if (maxdiff > 0.0 && difficulty > maxdiff)
    {
        printf("Current difficulty: %3.2f > maxdifficulty: %3.2f, sleeping for %d seconds...\n", difficulty, maxdiff,
            searchDuration / 1000);
        MilliSleep(searchDuration);
        return ret;
    }

    // ok, difficulty check passed or not applicable, proceed
#if 0
    UniValue tmp(UniValue::VOBJ);
    string tmpstr;
    std::vector<uint256> merkleproof;
    vector<unsigned char> coinbaseBytes(ParseHex(params["coinbase"].get_str()));


    // re-create merkle branches:
    {
        UniValue uvMerkleproof = params["merkleProof"];
        for (unsigned int i = 0; i < uvMerkleproof.size(); i++)
        {
            tmpstr = uvMerkleproof[i].get_str();
            std::vector<unsigned char> mbr = ParseHex(tmpstr);
            std::reverse(mbr.begin(), mbr.end());
            merkleproof.push_back(uint256(mbr));
        }
    }
#endif

    const CChainParams &cparams = Params();
    auto conp = cparams.GetConsensus();

    printf("Mining: id: %x headerCommitment: %s bits: %x difficulty: %3.4f\n", (unsigned int)params["id"].get_int64(),
        headerCommitment.ToString().c_str(), nBits, difficulty);

    int64_t start = GetTimeMillis();
    std::vector<unsigned char> nonce;
    int ChunkAmt = 1000;
    int checked = 0;
    while ((GetTimeMillis() < start + searchDuration) && !found && sharedBlkInfo == blkInfo)
    {
        // When mining mainnet, you would normally want to advance the time to keep the block time as close to the
        // real time as possible.  However, this CPU miner is only useful on testnet and in testnet the block difficulty
        // resets to 1 after 20 minutes.  This will cause the block's difficulty to mismatch the expected difficulty
        // and the block will be rejected.  So do not advance time (let it be advanced by bitcoind every time we
        // request a new block).
        // header.nTime = (header.nTime < GetTime()) ? GetTime() : header.nTime;
        int tries = ChunkAmt;
        found = CpuMineBlockHasherNextChain(tries, headerCommitment, nBits, randFunc, conp, nonce);
        checked += ChunkAmt - tries;
    }

    // Leave if not found:
    if (!found)
    {
        const int64_t elapsed = GetTimeMillis() - start;
        printf("Checked %d possibilities in %ld secs, %3.3f MH/s\n", checked, elapsed / 1000,
            (checked / 1e6) / (elapsed / 1e3));
        return ret;
    }

    printf("Solution! Checked %d possibilities\n", checked);

    UniValue tmp(UniValue::VOBJ);
    tmp.pushKV("id", params["id"]);
    tmp.pushKV("nonce", HexStr(nonce));
    ret.push_back(tmp);

    return ret;
}

static UniValue RPCSubmitSolution(const UniValue &solution, int &nblocks)
{
    UniValue reply = CallRPC("submitminingsolution", solution);

    const UniValue &error = find_value(reply, "error");

    if (!error.isNull())
    {
        fprintf(stderr, "Block Candidate submission error: %d %s\n", error["code"].get_int(),
            error["message"].get_str().c_str());
        return reply;
    }

    const UniValue &result = find_value(reply, "result");

    if (result.isNull())
    {
        printf("Unknown submission error, server gave no result\n");
    }
    else
    {
        const UniValue &errValue = find_value(result, "result");

        const UniValue &hashUV = find_value(result, "hash");
        std::string hashStr = hashUV.isNull() ? "" : hashUV.get_str();

        const UniValue &heightUV = find_value(result, "height");
        uint64_t height = heightUV.isNull() ? -1 : heightUV.get_int();


        if (errValue.isStr())
        {
            fprintf(stderr, "Block Candidate %s rejected. Error: %s\n", hashStr.c_str(), result.get_str().c_str());
            // Print some debug info if the block is rejected
            UniValue dbg = solution[0].get_obj();
            fprintf(stderr, "id: 0x%x  nonce: %s \n", dbg["id"].get_int(), dbg["nonce"].get_str().c_str());
        }
        else
        {
            if (errValue.isNull())
            {
                printf("Block Candidate %d:%s accepted.\n", height, hashStr.c_str());
                if (nblocks > 0)
                    nblocks--; // Processed a block
            }
            else
            {
                fprintf(stderr, "Unknown \"submitminingsolution\" Error.\n");
            }
        }
    }

    return reply;
}

int CpuMiner(void)
{
    // Initialize random number generator lambda. This is per-thread and
    // is thread-safe.  std::rand() is not thread-safe and can result
    // in multiple threads doing redundant proof-of-work.
    std::random_device rd;
    // seed random number generator from system entropy source (implementation defined: usually HW)
    std::default_random_engine e1(rd());
    // returns a uniformly distributed random number in the inclusive range: [0, UINT_MAX]
    std::uniform_int_distribution<uint32_t> uniformGen(0);
    auto randFunc = [&](void) -> uint32_t { return uniformGen(e1); };

    int searchDuration = GetArg("-duration", 30);
    int nblocks = GetArg("-nblocks", -1); //-1 mine forever
    int coinbasesize = GetArg("-coinbasesize", 0);
    std::string address = GetArg("-address", "");

    if (coinbasesize < 0)
    {
        printf("Negative coinbasesize not reasonable/supported.\n");
        return 0;
    }

    UniValue mineresult;
    bool found = false;

    if (0 == nblocks)
    {
        printf("Nothing to do for zero (0) blocks\n");
        return 0;
    }

    while (0 != nblocks)
    {
        UniValue reply;
        UniValue result;
        string strPrint;
        int nRet = 0;
        try
        {
            // Execute and handle connection failures with -rpcwait
            const bool fWait = true;
            do
            {
                try
                {
                    UniValue params(UniValue::VARR);
                    if (found)
                    {
                        // Submit the solution.
                        // Called here so all exceptions are handled properly below.
                        reply = RPCSubmitSolution(mineresult, nblocks);
                        if (nblocks == 0)
                            return 0; // Done mining exit program
                        found = false; // Mine again
                    }

                    if (!found)
                    {
                        if (coinbasesize > 0)
                        {
                            params.push_back(UniValue(coinbasesize));
                        }
                        if (!address.empty())
                        {
                            if (params.empty())
                            {
                                // param[0] must be coinbaseSize:
                                // push null in position 0 to use server default coinbaseSize
                                params.push_back(UniValue());
                            }
                            // this must be in position 1
                            params.push_back(UniValue(address));
                        }
                        reply = CallRPC("getminingcandidate", params);
                    }

                    // Parse reply
                    result = find_value(reply, "result");
                    const UniValue &error = find_value(reply, "error");

                    if (!error.isNull())
                    {
                        // Error
                        int code = error["code"].get_int();
                        if (fWait && code == RPC_IN_WARMUP)
                            throw CConnectionFailed("server in warmup");
                        strPrint = "error: " + error.write();
                        nRet = abs(code);
                        if (error.isObject())
                        {
                            UniValue errCode = find_value(error, "code");
                            UniValue errMsg = find_value(error, "message");
                            strPrint = errCode.isNull() ? "" : "error code: " + errCode.getValStr() + "\n";

                            if (errMsg.isStr())
                                strPrint += "error message:\n" + errMsg.get_str();
                        }
                    }
                    else
                    {
                        // Result
                        if (result.isNull())
                            strPrint = "";
                        else if (result.isStr())
                            strPrint = result.get_str();
                        else
                            strPrint = result.write(2);
                    }
                    // Connection succeeded, no need to retry.
                    break;
                }
                catch (const CConnectionFailed &c)
                {
                    if (fWait)
                    {
                        printf("Warning: %s\n", c.what());
                        MilliSleep(1000);
                    }
                    else
                        throw;
                }
            } while (fWait);
        }
        catch (const boost::thread_interrupted &)
        {
            throw;
        }
        catch (const std::exception &e)
        {
            strPrint = string("error: ") + e.what();
            nRet = EXIT_FAILURE;
        }
        catch (...)
        {
            PrintExceptionContinue(nullptr, "CommandLineRPC()");
            throw;
        }

        if (strPrint != "")
        {
            if (nRet != 0)
                fprintf(stderr, "%s\n", strPrint.c_str());
            // Actually do some mining
            if (result.isNull())
            {
                MilliSleep(1000);
            }
            else
            {
                found = false;
                mineresult = CpuMineBlock(searchDuration, result, found, randFunc);
                if (!found)
                {
                    // printf("Mining did not succeed\n");
                    mineresult.setNull();
                }
                // The result is sent to bitcoind above when the loop gets to it.
                // See:   RPCSubmitSolution(mineresult,nblocks);
                // This is so RPC Exceptions are handled in one place.
            }
        }
    }
    return 0;
}

void static MinerThread()
{
    while (1)
    {
        try
        {
            CpuMiner();
        }
        catch (const std::exception &e)
        {
            PrintExceptionContinue(&e, "CommandLineRPC()");
        }
        catch (...)
        {
            PrintExceptionContinue(nullptr, "CommandLineRPC()");
        }
    }
}

int main(int argc, char *argv[])
{
    Secp256k1Init secp;
    SetupEnvironment();
    if (!SetupNetworking())
    {
        fprintf(stderr, "Error: Initializing networking failed\n");
        exit(1);
    }

    try
    {
        std::string appname("bitcoin-miner");
        std::string usage = "\n" + _("Usage:") + "\n" + "  " + appname + " [options] " + "\n";
        int ret = AppInitRPC(usage, BitcoinMinerArgs(), argc, argv);
        if (ret != CONTINUE_EXECUTION)
            return ret;
    }
    catch (const std::exception &e)
    {
        PrintExceptionContinue(&e, "AppInitRPC()");
        return EXIT_FAILURE;
    }
    catch (...)
    {
        PrintExceptionContinue(nullptr, "AppInitRPC()");
        return EXIT_FAILURE;
    }
    SelectParams(ChainNameFromCommandLine());

    int nThreads = GetArg("-cpus", 1);
    boost::thread_group minerThreads;
    printf("Running on %d CPUs\n", nThreads);
    for (int i = 0; i < nThreads - 1; i++)
        minerThreads.create_thread(MinerThread);

    int ret = EXIT_FAILURE;
    try
    {
        ret = CpuMiner();
    }
    catch (const std::exception &e)
    {
        PrintExceptionContinue(&e, "CommandLineRPC()");
    }
    catch (...)
    {
        PrintExceptionContinue(nullptr, "CommandLineRPC()");
    }
    return ret;
}