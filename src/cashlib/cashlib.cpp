// Copyright (c) 2015-2020 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
/* clang-format off */
// must be first for windows
#include "compat.h"
/* clang-format on */

#ifdef ANDROID // Workaround to fix gradle build
#define SECP256K1_INLINE inline
#endif

#include "arith_uint256.h"
#include "base58.h"
#include "bloom.h"
#include "cashaddrenc.h"
#include "chainparams.h"
#include "coins.h"
#include "consensus/validation.h"
#include "merkleblock.h"
#include "random.h"
#include "script/sign.h"
#include "streams.h"
#include "tinyformat.h"
#include "uint256.h"
#include "util.h"
#include "utilstrencodings.h"
#define MAX_SIG_LEN 100 // DER-encoded ECDSA is more like 72 but better to be safe

#ifdef DEBUG_LOCKORDER // Not debugging the lockorder in cashlib even if its defined
void AssertLockHeldInternal(const char *pszName, const char *pszFile, unsigned int nLine, void *cs) {}
void AssertLockNotHeldInternal(const char *pszName, const char *pszFile, unsigned int nLine, void *cs) {}
void EnterCritical(const char *pszName,
    const char *pszFile,
    unsigned int nLine,
    void *cs,
    LockType locktype,
    OwnershipType ownership,
    bool fTry)
{
}
void LeaveCritical(void *cs) {}
void AssertWriteLockHeldInternal(const char *pszName,
    const char *pszFile,
    unsigned int nLine,
    CSharedCriticalSection *cs)
{
}
void AssertRecursiveWriteLockHeldInternal(const char *pszName,
    const char *pszFile,
    unsigned int nLine,
    CRecursiveSharedCriticalSection *cs)
{
}
CCriticalSection::CCriticalSection() : name(nullptr) {}
CCriticalSection::CCriticalSection(const char *n) : name(n) {}
CCriticalSection::~CCriticalSection() {}
CSharedCriticalSection::CSharedCriticalSection() : name(nullptr) {}
CSharedCriticalSection::CSharedCriticalSection(const char *n) : name(n) {}
CSharedCriticalSection::~CSharedCriticalSection() {}
CRecursiveSharedCriticalSection::CRecursiveSharedCriticalSection() : name(nullptr) {}
CRecursiveSharedCriticalSection::CRecursiveSharedCriticalSection(const char *n) : name(n) {}
CRecursiveSharedCriticalSection::~CRecursiveSharedCriticalSection() {}
#endif

#ifndef ANDROID
#include <openssl/rand.h>
#endif

#include <algorithm>
#include <string>
#include <vector>

static bool sigInited = false;

ECCVerifyHandle *verifyContext = nullptr;
CChainParams *cashlibParams = nullptr;
#ifdef DEBUG_PAUSE
bool pauseOnDbgAssert = false;
std::mutex dbgPauseMutex;
std::condition_variable dbgPauseCond;
void DbgPause()
{
#ifdef __linux__ // The thread ID returned by gettid is very useful since its shown in gdb
    printf("\n!!! Process %d, Thread %ld (%lx) paused !!!\n", getpid(), syscall(SYS_gettid), pthread_self());
#else
    printf("\n!!! Process %d paused !!!\n", getpid());
#endif
    std::unique_lock<std::mutex> lk(dbgPauseMutex);
    dbgPauseCond.wait(lk);
}

extern "C" void DbgResume() { dbgPauseCond.notify_all(); }
#endif
#ifdef ANDROID // log sighash calculations
#include <android/log.h>
#define p(...) __android_log_print(ANDROID_LOG_DEBUG, "bu.sig", __VA_ARGS__)
#else
#define p(...)
// tinyformat::format(std::cout, __VA_ARGS__)
#endif

const int CLIENT_VERSION = 0; // 0 because app should report its version, not this lib

// stop the logging
int LogPrintStr(const std::string &str) { return str.size(); }
namespace Logging
{
uint64_t categoriesEnabled = 0; // 64 bit log id mask.
};

// I don't want to pull in the args stuff so always pick the defaults
bool GetBoolArg(const std::string &strArg, bool fDefault) { return fDefault; }
// cashlib does not support versionbits right now so just supply this which is used in chainparams
struct ForkDeploymentInfo
{
    /** Deployment name */
    const char *name;
    /** Whether GBT clients can safely ignore this rule in simplified usage */
    bool gbt_force;
    /** What is this client's vote? */
    bool myVote;
};
struct ForkDeploymentInfo VersionBitsDeploymentInfo[Consensus::MAX_VERSION_BITS_DEPLOYMENTS];

// Must match the equivalent object in calling language code (e.g. PayAddressType)
typedef enum
{
    PayAddressTypeNONE = 0,
    PayAddressTypeP2PUBKEY = 1,
    PayAddressTypeP2PKH = 2,
    PayAddressTypeP2SH = 3,
    PayAddressTypeTEMPLATE = 4, // Generalized pay to script template
    PayAddressTypeP2PKT = 5 // Pay to well-known script template 1 (pay-to-pub-key-template)
} PayAddressType;

// Must match the equivalent object in calling language code (e.g. ChainSelector)
typedef enum
{
    AddrBlockchainNexa = 1,
    AddrBlockchainTestnet = 2,
    AddrBlockchainRegtest = 3,
    AddrBlockchainBCH = 4
} ChainSelector;

CChainParams *GetChainParams(ChainSelector chainSelector)
{
    if (chainSelector == AddrBlockchainNexa)
        return &Params(CBaseChainParams::NEXTCHAIN);
    else if (chainSelector == AddrBlockchainTestnet)
        return &Params(CBaseChainParams::TESTNET);
    else if (chainSelector == AddrBlockchainRegtest)
        return &Params(CBaseChainParams::REGTEST);
    else if (chainSelector == AddrBlockchainBCH)
        return &Params(CBaseChainParams::LEGACY_UNIT_TESTS);
    else
        return nullptr;
}

// No-op this RPC function that is unused in .so context
extern UniValue token(const UniValue &params, bool fHelp) { return UniValue(); }
// helper functions
namespace
{
void checkSigInit()
{
    if (!sigInited)
    {
        sigInited = true;
        SHA256AutoDetect();
        ECC_Start();
        verifyContext = new ECCVerifyHandle();
    }
}

CKey LoadKey(unsigned char *src)
{
    CKey secret;
    checkSigInit();
    secret.Set(src, src + 32, true);
    return secret;
}

#if 0
// This function is temporarily removed since it is not used.  However it will be needed for interfacing to
// languages that handle binary data poorly, since it allows transaction information to be communicated via hex strings

// From core_read.cpp #include "core_io.h"
bool DecodeHexTx(CTransaction &tx, const std::string &strHexTx)
{
    if (!IsHex(strHexTx))
        return false;

    std::vector<unsigned char> txData(ParseHex(strHexTx));
    CDataStream ssData(txData, SER_NETWORK, PROTOCOL_VERSION);
    try
    {
        ssData >> tx;
    }
    catch (const std::exception &)
    {
        return false;
    }

    return true;
}
#endif
} // namespace

/** Convert binary data to a hex string.  The provided result buffer must be 2*length+1 bytes.
 */
SLAPI int Bin2Hex(unsigned char *val, int length, char *result, unsigned int resultLen)
{
    std::string s = GetHex(val, length);
    if (s.size() >= resultLen)
        return 0; // need 1 more for /0
    strncpy(result, s.c_str(), resultLen);
    return 1;
}

/** Given a private key, return its corresponding public key */
SLAPI int GetPubKey(unsigned char *keyData, unsigned char *result, unsigned int resultLen)
{
    checkSigInit();
    CKey key = LoadKey(keyData);
    CPubKey pubkey = key.GetPubKey();
    unsigned int size = pubkey.size();
    if (size > resultLen)
        return 0;
    std::copy(pubkey.begin(), pubkey.end(), result);
    return size;
}

/** Sign data (compatible with OP_CHECKDATASIG) */
SLAPI int SignHashEDCSA(unsigned char *data,
    int datalen,
    unsigned char *secret,
    unsigned char *result,
    unsigned int resultLen)
{
    checkSigInit();
    CKey key = LoadKey(secret);
    uint256 hash;
    CSHA256().Write(data, datalen).Finalize(hash.begin());
    std::vector<uint8_t> sig;
    if (!key.SignECDSA(hash, sig))
    {
        return 0;
    }
    unsigned int sigSize = sig.size();
    if (sigSize > resultLen)
        return 0;
    std::copy(sig.begin(), sig.end(), result);
    return sigSize;
}

SLAPI int txid(unsigned char *txData, int txbuflen, unsigned char *result)
{
    CTransaction tx;
    CDataStream ssData((char *)txData, (char *)txData + txbuflen, SER_NETWORK, PROTOCOL_VERSION);
    try
    {
        ssData >> tx;
    }
    catch (const std::exception &)
    {
        return 0;
    }
    uint256 ret = tx.GetId();
    memcpy(result, ret.begin(), ret.size());
    return 1;
}

SLAPI int txidem(unsigned char *txData, int txbuflen, unsigned char *result)
{
    CTransaction tx;
    CDataStream ssData((char *)txData, (char *)txData + txbuflen, SER_NETWORK, PROTOCOL_VERSION);
    try
    {
        ssData >> tx;
    }
    catch (const std::exception &)
    {
        return 0;
    }
    uint256 ret = tx.GetIdem();
    memcpy(result, ret.begin(), ret.size());
    return 1;
}

/** Sign one input of a transaction
    All buffer arguments should be in binary-serialized data.
    The transaction (txData) must contain the COutPoint (tx hash and vout) of all relevant inputs,
    however, it is not necessary to provide the spend script.
    Returns length of returned signature.
*/
SLAPI int SignTxECDSA(unsigned char *txData,
    int txbuflen,
    unsigned int inputIdx,
    int64_t inputAmount,
    unsigned char *prevoutScript,
    uint32_t priorScriptLen,
    uint32_t nHashType,
    unsigned char *keyData,
    unsigned char *result,
    unsigned int resultLen)
{
    DbgAssert(nHashType & BTCBCH_SIGHASH_FORKID, return 0);
    uint8_t sigHashType(nHashType);
    checkSigInit();
    CTransaction tx;
    result[0] = 0;

    CDataStream ssData((char *)txData, (char *)txData + txbuflen, SER_NETWORK, PROTOCOL_VERSION);
    try
    {
        ssData >> tx;
    }
    catch (const std::exception &)
    {
        return 0;
    }

    if (inputIdx >= tx.vin.size())
        return 0;

    CScript priorScript(prevoutScript, prevoutScript + priorScriptLen);
    CKey key = LoadKey(keyData);

    size_t nHashedOut = 0;
    uint256 sighash = SignatureHashBitcoinCash(priorScript, tx, inputIdx, sigHashType, inputAmount, &nHashedOut);
    std::vector<unsigned char> sig;
    if (!key.SignECDSA(sighash, sig))
    {
        return 0;
    }
    sig.push_back(sigHashType);
    unsigned int sigSize = sig.size();
    if (sigSize > resultLen)
        return 0;
    std::copy(sig.begin(), sig.end(), result);
    return sigSize;
}

/** Sign one input of a transaction
    All buffer arguments should be in binary-serialized data.
    The transaction (txData) must contain the COutPoint (tx hash and vout) of all relevant inputs,
    however, it is not necessary to provide the spend script.
*/
SLAPI int SignBchTxSchnorr(unsigned char *txData,
    int txbuflen,
    unsigned int inputIdx,
    int64_t inputAmount,
    unsigned char *prevoutScript,
    uint32_t priorScriptLen,
    uint32_t nHashType,
    unsigned char *keyData,
    unsigned char *result,
    unsigned int resultLen)
{
    DbgAssert(nHashType & BTCBCH_SIGHASH_FORKID, return 0);
    uint8_t sigHashType = nHashType;
    checkSigInit();
    CTransaction tx;
    result[0] = 0;

    CDataStream ssData((char *)txData, (char *)txData + txbuflen, SER_NETWORK, PROTOCOL_VERSION);
    try
    {
        ssData >> tx;
    }
    catch (const std::exception &)
    {
        return 0;
    }

    if (inputIdx >= tx.vin.size())
        return 0;

    CScript priorScript(prevoutScript, prevoutScript + priorScriptLen);
    CKey key = LoadKey(keyData);

    size_t nHashedOut = 0;
    uint256 sighash = SignatureHashBitcoinCash(priorScript, tx, inputIdx, sigHashType, inputAmount, &nHashedOut);
    std::vector<unsigned char> sig;
    if (!key.SignSchnorr(sighash, sig))
    {
        return 0;
    }
    // CPubKey pub = key.GetPubKey();
    // p("Sign BCH Schnorr: sig: %s, pubkey: %s sighash: %s\n", HexStr(sig).c_str(),
    //    HexStr(pub.begin(), pub.end()).c_str(), sighash.GetHex().c_str());
    sig.push_back(sigHashType);
    unsigned int sigSize = sig.size();
    if (sigSize > resultLen)
        return 0;
    std::copy(sig.begin(), sig.end(), result);
    return sigSize;
}

/** Sign one input of a transaction
    All buffer arguments should be in binary-serialized data.
    The transaction (txData) must contain the COutPoint (tx hash and vout) of all relevant inputs,
    however, it is not necessary to provide the spend script.
*/
SLAPI int SignTxSchnorr(unsigned char *txData,
    int txbuflen,
    unsigned int inputIdx,
    int64_t inputAmount,
    unsigned char *prevoutScript,
    uint32_t priorScriptLen,
    unsigned char *hashType,
    unsigned int hashTypeLen,
    unsigned char *keyData,
    unsigned char *result,
    unsigned int resultLen)
{
    checkSigInit();
    CTransaction tx;
    result[0] = 0;

    std::vector<uint8_t> sigHashVec(hashType, hashType + hashTypeLen);
    SigHashType sigHashType(sigHashVec);

    CDataStream ssData((char *)txData, (char *)txData + txbuflen, SER_NETWORK, PROTOCOL_VERSION);
    try
    {
        ssData >> tx;
    }
    catch (const std::exception &)
    {
        return 0;
    }

    if (inputIdx >= tx.vin.size())
        return 0;

    CScript priorScript(prevoutScript, prevoutScript + priorScriptLen);
    CKey key = LoadKey(keyData);

    size_t nHashedOut = 0;
    uint256 sighash = SignatureHash(priorScript, tx, inputIdx, sigHashType, inputAmount, &nHashedOut);
    std::vector<unsigned char> sig;
    if (!key.SignSchnorr(sighash, sig))
    {
        return 0;
    }
    // p("Sign Schnorr: sig: %s, pubkey: %s sighash: %s\n", HexStr(sig).c_str(), HexStr(pub.begin(), pub.end()).c_str(),
    // sighash.GetHex().c_str());
    sigHashType.appendToSig(sig);
    unsigned int sigSize = sig.size();
    if (sigSize > resultLen)
        return 0;
    std::copy(sig.begin(), sig.end(), result);
    return sigSize;
}

/** Sign data via the Schnorr signature algorithm.  hash must be 32 bytes.
    All buffer arguments should be in binary-serialized data.
    The transaction (txData) must contain the COutPoint (tx hash and vout) of all relevant inputs,
    however, it is not necessary to provide the spend script.

    The returned signature will not have a sighashtype byte.
*/
SLAPI int SignHashSchnorr(const unsigned char *hash,
    unsigned char *keyData,
    unsigned char *result,
    unsigned int resultLen)
{
    uint256 sighash(hash);
    std::vector<unsigned char> sig;
    checkSigInit();

    CKey key = LoadKey(keyData);

    if (!key.SignSchnorr(sighash, sig))
    {
        return 0;
    }
    unsigned int sigSize = sig.size();
    if (sigSize > resultLen)
        return 0;
    std::copy(sig.begin(), sig.end(), result);
    return sigSize;
}

#ifndef ANDROID
/*
Since the ScriptMachine is often going to be initialized, called and destructed within a single stack frame, it
does not make copies of the data it is using.  But higher-level language and debugging interaction use the
ScriptMachine across stack frames.  Therefore it is necessary to create a class to hold all of this data on behalf
of the ScriptMachine.
 */
class ScriptMachineData
{
public:
    ScriptMachineData() : sm(nullptr), tx(nullptr), sis(nullptr), script(nullptr) {}
    ScriptMachine *sm;

    CTransactionRef tx;
    std::shared_ptr<BaseSignatureChecker> checker;
    std::shared_ptr<ScriptImportedState> sis;
    std::shared_ptr<CScript> script;

    ~ScriptMachineData()
    {
        if (sm)
        {
            delete sm;
            sm = nullptr;
        }
    }
};

// Create a ScriptMachine with no transaction context -- useful for tests and debugging
// This ScriptMachine can't CHECKSIG or CHECKSIGVERIFY
SLAPI void *CreateNoContextScriptMachine(unsigned int flags)
{
    ScriptMachineData *smd = new ScriptMachineData();
    smd->sis = std::make_shared<ScriptImportedState>();
    smd->sm = new ScriptMachine(flags, *smd->sis, 0xffffffff, 0xffffffff);
    return (void *)smd;
}

// Create a ScriptMachine operating in the context of a particular transaction and input.
// The transaction, input index, and input amount are used in CHECKSIG and CHECKSIGVERIFY to generate the hash that
// the signature validates.
SLAPI void *CreateScriptMachine(unsigned int flags,
    unsigned int inputIdx,
    unsigned char *txData,
    int txbuflen,
    unsigned char *coinData,
    int coinbuflen)
{
    checkSigInit();

    ScriptMachineData *smd = new ScriptMachineData();
    std::shared_ptr<CTransaction> txref = std::make_shared<CTransaction>();
    std::vector<CTxOut> coins;

    {
        CDataStream ssData((char *)txData, (char *)txData + txbuflen, SER_NETWORK, PROTOCOL_VERSION);
        try
        {
            ssData >> *txref;
        }
        catch (const std::exception &)
        {
            delete smd;
            return 0;
        }
    }

    {
        CDataStream ssData((char *)coinData, (char *)coinData + coinbuflen, SER_NETWORK, PROTOCOL_VERSION);
        try
        {
            ssData >> coins;
        }
        catch (const std::exception &)
        {
            delete smd;
            return 0;
        }
    }

    // The passed coins vector needs to be the txout for each vin, so the sizes must be the same
    if (coins.size() != txref->vin.size())
    {
        delete smd;
        return 0;
    }

    CValidationState state;
    {
        // Construct a view of all the supplied coins
        CCoinsView coinsDummy;
        CCoinsViewCache prevouts(&coinsDummy);
        for (size_t i = 0; i < coins.size(); i++)
        {
            // We assume that the passed coins are in the proper order so their outpoint is what is specified
            // in the tx.  We further assume height 1 and not coinbase.  These fields are not accessible from scripts
            // so should not affect execution.
            prevouts.AddCoin(txref->vin[i].prevout, Coin(coins[i], 1, false), false);
        }

        // Fill the validation state with derived data about this transaction
        /* This pulls in too much stuff (in particular it needs to determine input coin height,
           to check coinbase spendability, which requires knowing the tip height).
           Think about refactoring CheckTxInputs to take the tip height as a parameter for functional isolation
           For now, calculate the needed data directly.
           Leaving this "canonical" code in for reference purposes
        if (!Consensus::CheckTxInputs(txref, state, prevouts))
        {
            delete smd;
            return 0;
        }
        */
        CAmount amountIn = 0;
        for (size_t i = 0; i < txref->vin.size(); i++)
        {
            amountIn += txref->vin[i].amount;
        }
        CAmount amountOut = 0;
        for (size_t i = 0; i < txref->vout.size(); i++)
        {
            amountOut += txref->vout[i].nValue;
        }
        state.inAmount = amountIn;
        state.outAmount = amountOut;
        state.fee = amountIn - amountOut;
        if (!CheckGroupTokens(*txref, state, prevouts))
        {
            delete smd;
            return 0;
        }
    }

    smd->tx = txref;
    // Its ok to get the bare tx pointer: the life of the CTransaction is the same as TransactionSignatureChecker
    // -1 is the inputAmount -- no longer used
    smd->checker = std::make_shared<TransactionSignatureChecker>(smd->tx.get(), inputIdx, flags);
    smd->sis = std::make_shared<ScriptImportedState>(&(*smd->checker), smd->tx, state, coins, inputIdx);
    // max ops and max sigchecks are set to the maximum value with the intention that the caller will check these if
    // needed because many uses of the script machine are for debugging and experimental scripts.
    smd->sm = new ScriptMachine(flags, *smd->sis, 0xffffffff, 0xffffffff);
    return (void *)smd;
}

// Release a ScriptMachine context
SLAPI void SmRelease(void *smId)
{
    ScriptMachineData *smd = (ScriptMachineData *)smId;
    if (!smd)
        return;
    delete smd;
}

// Copy the provided ScriptMachine, returning a new ScriptMachine id that exactly matches the current one
SLAPI void *SmClone(void *smId)
{
    ScriptMachineData *from = (ScriptMachineData *)smId;
    ScriptMachineData *to = new ScriptMachineData();
    to->script = from->script;
    to->sis = from->sis;
    to->tx = from->tx;
    to->sis->tx = to->tx; // Get it pointing to the right object even though they are currently the same
    to->sm = new ScriptMachine(*from->sm);
    return (void *)to;
}


// Evaluate a script within the context of this script machine
SLAPI bool SmEval(void *smId, unsigned char *scriptBuf, unsigned int scriptLen)
{
    ScriptMachineData *smd = (ScriptMachineData *)smId;

    CScript script(scriptBuf, scriptBuf + scriptLen);
    bool ret = smd->sm->Eval(script);
    return ret;
}

// Step-by-step interface: start evaluating a script within the context of this script machine
SLAPI bool SmBeginStep(void *smId, unsigned char *scriptBuf, unsigned int scriptLen)
{
    ScriptMachineData *smd = (ScriptMachineData *)smId;
    // shared_ptr will auto-release the old one
    smd->script = std::make_shared<CScript>(scriptBuf, scriptBuf + scriptLen);
    bool ret = smd->sm->BeginStep(*smd->script);
    return ret;
}

// Step-by-step interface: execute the next instruction in the script
SLAPI unsigned int SmStep(void *smId)
{
    ScriptMachineData *smd = (ScriptMachineData *)smId;
    unsigned int ret = smd->sm->Step();
    return ret;
}

// Step-by-step interface: get the current position in this script, specified in bytes offset from the script start
SLAPI int SmPos(void *smId)
{
    ScriptMachineData *smd = (ScriptMachineData *)smId;
    return smd->sm->getPos();
}


// Step-by-step interface: End script evaluation
SLAPI bool SmEndStep(void *smId)
{
    ScriptMachineData *smd = (ScriptMachineData *)smId;
    bool ret = smd->sm->EndStep();
    return ret;
}


// Revert the script machine to initial conditions
SLAPI void SmReset(void *smId)
{
    ScriptMachineData *smd = (ScriptMachineData *)smId;
    smd->sm->Reset();
}


// Get a stack item, 0 = stack, 1 = altstack,  pass a buffer at least 520 bytes in size
// returns length of the item or -1 if no item.  0 is the stack top
SLAPI void SmSetStackItem(void *smId,
    unsigned int stack,
    int index,
    StackElementType t,
    const unsigned char *value,
    unsigned int valsize)
{
    ScriptMachineData *smd = (ScriptMachineData *)smId;

    const std::vector<StackItem> &stk = (stack == 0) ? smd->sm->getStack() : smd->sm->getAltStack();
    if (((int)stk.size()) <= index)
        return;

    StackItem si;
    if (t == StackElementType::VCH)
    {
        si = StackItem(value, value + valsize);
    }
    else if (t == StackElementType::BIGNUM)
    {
        BigNum bn;
        bn.deserialize(value, valsize);
        si = StackItem(bn);
    }
    else
    {
        return;
    }

    if (stack == 0)
    {
        smd->sm->setStackItem(index, si);
    }
    else if (stack == 1)
    {
        smd->sm->setAltStackItem(index, si);
    }
}

// Get a stack item, 0 = stack, 1 = altstack,  pass a buffer at least 520 bytes in size
// returns length of the item or -1 if no item.  0 is the stack top
SLAPI int SmGetStackItem(void *smId, unsigned int stack, unsigned int index, StackElementType *t, unsigned char *result)
{
    ScriptMachineData *smd = (ScriptMachineData *)smId;

    const std::vector<StackItem> &stk = (stack == 0) ? smd->sm->getStack() : smd->sm->getAltStack();
    if (stk.size() <= index)
        return -1;
    index = stk.size() - index - 1; // reverse it so 0 is stack top

    const StackItem &item = stk[index];

    *t = item.type;
    if (item.type == StackElementType::VCH)
    {
        int sz = stk[index].size();
        memcpy(result, stk[index].data().data(), sz);
        return sz;
    }
    else if (item.type == StackElementType::BIGNUM)
    {
        int sz = item.num().serialize(result, 512);
        return (sz);
    }
    else
        return 0;
}

// Returns the last error generated during script evaluation (if any)
SLAPI unsigned int SmGetError(void *smId)
{
    ScriptMachineData *smd = (ScriptMachineData *)smId;
    return (unsigned int)smd->sm->getError();
}
#endif

// result must be 32 bytes
SLAPI void sha256(const unsigned char *data, unsigned int len, unsigned char *result)
{
    CSHA256 sha;
    sha.Write(data, len);
    sha.Finalize(result);
}


// result must be 32 bytes
SLAPI void hash256(const unsigned char *data, unsigned int len, unsigned char *result)
{
    CHash256 hash;
    hash.Write(data, len);
    hash.Finalize(result);
}


// result must be 20 bytes
SLAPI void hash160(const unsigned char *data, unsigned int len, unsigned char *result)
{
    CHash160 hash;
    hash.Write(data, len);
    hash.Finalize(result);
}


#ifdef ANDROID
#include <android/log.h>
#else

#ifdef JAVA
#define __android_log_print(x, y, z, ...) \
    do                                    \
    {                                     \
    } while (0)
#endif

#endif

#ifdef JAVA
#include <jni.h>

#define APPNAME "BU.wallet.cashlib"

jclass secRandomClass = nullptr;
jmethodID secRandom = nullptr;
JNIEnv *javaEnv = nullptr; // Only use for getting random numbers

class ByteArrayAccessor
{
public:
    JNIEnv *env;
    jbyteArray &obj;
    uint8_t *data;
    size_t size;

    std::vector<uint8_t> vec() { return std::vector<uint8_t>(data, data + size); }
    ByteArrayAccessor(JNIEnv *e, jbyteArray &arg) : env(e), obj(arg)
    {
        size = env->GetArrayLength(obj);
        data = (uint8_t *)env->GetByteArrayElements(obj, nullptr);
    }

    ~ByteArrayAccessor()
    {
        size = 0;
        if (data)
            env->ReleaseByteArrayElements(obj, (jbyte *)data, 0);
    }
};

// credit: https://stackoverflow.com/questions/41820039/jstringjni-to-stdstringc-with-utf8-characters
std::string toString(JNIEnv *env, jstring jStr)
{
    if (!jStr)
        return "";

    const jclass stringClass = env->GetObjectClass(jStr);
    const jmethodID getBytes = env->GetMethodID(stringClass, "getBytes", "(Ljava/lang/String;)[B");
    const jbyteArray stringJbytes = (jbyteArray)env->CallObjectMethod(jStr, getBytes, env->NewStringUTF("UTF-8"));

    size_t length = (size_t)env->GetArrayLength(stringJbytes);
    jbyte *pBytes = env->GetByteArrayElements(stringJbytes, nullptr);

    std::string ret = std::string((char *)pBytes, length);
    env->ReleaseByteArrayElements(stringJbytes, pBytes, JNI_ABORT);

    env->DeleteLocalRef(stringJbytes);
    env->DeleteLocalRef(stringClass);
    return ret;
}

jint triggerJavaIllegalStateException(JNIEnv *env, const char *message)
{
    jclass exc = env->FindClass("java/lang/IllegalStateException");
    if (nullptr == exc)
        return 0;
    return env->ThrowNew(exc, message);
}

/** converts a arith_uint256 into something that java BigInteger can grab */
jbyteArray encodeUint256(JNIEnv *env, arith_uint256 value)
{
    const size_t size = 256 / 8;
    jbyteArray result = env->NewByteArray(size);
    if (result != nullptr)
    {
        jbyte *data = env->GetByteArrayElements(result, nullptr);
        if (data != nullptr)
        {
            int i;
            for (i = (int)(size - 1); i >= 0; i--)
            {
                data[i] = (jbyte)(value.GetLow64() & 0xFF);
                value >>= 8;
            }
            env->ReleaseByteArrayElements(result, data, 0);
        }
    }
    return result;
}


jbyteArray makeJByteArray(JNIEnv *env, uint8_t *buf, size_t size)
{
    jbyteArray bArray = env->NewByteArray(size);
    jbyte *dest = env->GetByteArrayElements(bArray, 0);
    memcpy(dest, buf, size);
    env->ReleaseByteArrayElements(bArray, dest, 0);
    return bArray;
}

jbyteArray makeJByteArray(JNIEnv *env, std::string &buf)
{
    jbyteArray bArray = env->NewByteArray(buf.size());
    jbyte *dest = env->GetByteArrayElements(bArray, 0);
    memcpy(dest, buf.c_str(), buf.size());
    env->ReleaseByteArrayElements(bArray, dest, 0);
    return bArray;
}

jbyteArray makeJByteArray(JNIEnv *env, std::vector<unsigned char> &buf)
{
    jbyteArray bArray = env->NewByteArray(buf.size());
    jbyte *dest = env->GetByteArrayElements(bArray, 0);
    memcpy(dest, &buf[0], buf.size());
    env->ReleaseByteArrayElements(bArray, dest, 0);
    return bArray;
}


extern "C" JNIEXPORT jbyteArray JNICALL Java_bitcoinunlimited_libbitcoincash_Wallet_signMessage(JNIEnv *env,
    jobject ths,
    jbyteArray jmessage,
    jbyteArray secret)
{
    ByteArrayAccessor message(env, jmessage);
    ByteArrayAccessor privkey(env, secret);
    if (privkey.size != 32)
        return jbyteArray();

    checkSigInit();

    CKey key = LoadKey((unsigned char *)privkey.data);

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic << message.vec();

    uint256 msgHash = ss.GetHash();
    // __android_log_print(ANDROID_LOG_INFO, APPNAME, "signing msgHash %s\n", msgHash.GetHex().c_str());
    std::vector<unsigned char> vchSig;
    if (!key.SignCompact(msgHash, vchSig)) // signing will only fail if the key is bogus
    {
        return jbyteArray();
    }
    if (vchSig.size() == 0)
        return jbyteArray();

    // __android_log_print(ANDROID_LOG_INFO, APPNAME, "signing sigSize %d data %s\n", vchSig.size(),
    // GetHex(vchSig.begin(), vchSig.size()).c_str());
    return makeJByteArray(env, vchSig);
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_bitcoinunlimited_libbitcoincash_Wallet_verifyMessage(JNIEnv *env,
    jobject ths,
    jbyteArray jmessage,
    jbyteArray addrBytes,
    jbyteArray sigBytes)
{
    ByteArrayAccessor message(env, jmessage);
    ByteArrayAccessor addr(env, addrBytes);
    ByteArrayAccessor sig(env, sigBytes);
    if (addr.size != 20)
        return jbyteArray();

    checkSigInit();

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic << message.vec();

    uint256 msgHash = ss.GetHash();
    //__android_log_print(ANDROID_LOG_INFO, APPNAME, "verifying msgHash %s\n", msgHash.GetHex().c_str());
    //__android_log_print(ANDROID_LOG_INFO, APPNAME, "verifying sigSize %d data %s\n", sig.size, GetHex(sig.data,
    // sig.size).c_str());

    CPubKey pubkey;
    if (!pubkey.RecoverCompact(msgHash, sig.vec()))
        return jbyteArray();

    CKeyID pkAddr = pubkey.GetID();
    CKeyID passedAddr = CKeyID(uint160(addr.data));
    //__android_log_print(ANDROID_LOG_INFO, APPNAME, "pkAddr %s\n", pkAddr.GetHex().c_str());
    //__android_log_print(ANDROID_LOG_INFO, APPNAME, "passedAddr %s\n", passedAddr.GetHex().c_str());
    if (pkAddr == passedAddr)
    {
        auto pkv = std::vector<unsigned char>(pubkey.begin(), pubkey.end());
        return makeJByteArray(env, pkv);
    }

    return jbyteArray();
}


extern "C" JNIEXPORT jstring JNICALL Java_bitcoinunlimited_libbitcoincash_Codec_encode64(JNIEnv *env,
    jobject ths,
    jbyteArray jdata)
{
    ByteArrayAccessor data(env, jdata);
    auto dataAsStr = EncodeBase64(data.data, data.size);
    return env->NewStringUTF(dataAsStr.c_str());
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_bitcoinunlimited_libbitcoincash_Codec_decode64(JNIEnv *env,
    jobject ths,
    jstring jdata)
{
    std::string data = toString(env, jdata);
    bool invalid = true;
    auto dataBytes = DecodeBase64(data.c_str(), &invalid);
    if (invalid)
    {
        triggerJavaIllegalStateException(env, "bad encoding");
        return jbyteArray();
    }
    return makeJByteArray(env, dataBytes);
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_bitcoinunlimited_libbitcoincash_Wallet_signOneInputUsingECDSA(JNIEnv *env,
    jobject ths,
    jbyteArray txData,
    jint sigHashType,
    jlong inputIdx,
    jlong inputAmount,
    jbyteArray prevoutScript,
    jbyteArray secret)
{
    ByteArrayAccessor tx(env, txData);
    ByteArrayAccessor prevout(env, prevoutScript);
    ByteArrayAccessor privkey(env, secret);
    if (privkey.size != 32)
        return jbyteArray();

    unsigned char result[MAX_SIG_LEN];
    uint32_t resultLen = SignTxECDSA(tx.data, tx.size, inputIdx, inputAmount, prevout.data, prevout.size, sigHashType,
        privkey.data, result, MAX_SIG_LEN);

    if (resultLen == 0)
        return jbyteArray();
    return makeJByteArray(env, result, resultLen);
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_bitcoinunlimited_libbitcoincash_Wallet_signOneInputUsingSchnorr(
    JNIEnv *env,
    jobject ths,
    jbyteArray txData,
    unsigned char *hashType,
    unsigned int hashTypeLen,
    jlong inputIdx,
    jlong inputAmount,
    jbyteArray prevoutScript,
    jbyteArray secret)
{
    ByteArrayAccessor tx(env, txData);
    ByteArrayAccessor prevout(env, prevoutScript);
    ByteArrayAccessor privkey(env, secret);
    if (privkey.size != 32)
        return jbyteArray();

    unsigned char result[MAX_SIG_LEN];
    uint32_t resultLen = SignTxSchnorr(tx.data, tx.size, inputIdx, inputAmount, prevout.data, prevout.size, hashType,
        hashTypeLen, privkey.data, result, MAX_SIG_LEN);

    if (resultLen == 0)
        return jbyteArray();
    return makeJByteArray(env, result, resultLen);
}

/** Create a bloom filter */
extern "C" JNIEXPORT jbyteArray JNICALL Java_bitcoinunlimited_libbitcoincash_Wallet_CreateBloomFilter(JNIEnv *env,
    jobject ths,
    jobjectArray arg,
    jdouble falsePosRate,
    jint capacity,
    jint maxSize,
    jint flags,
    jint tweak)
{
    jclass byteArrayClass = env->FindClass("[B");
    size_t len = env->GetArrayLength(arg);
    if (capacity < 10)
        capacity = 10; // sanity check the capacity

    if (!((falsePosRate >= 0) && (falsePosRate <= 1.0)))
    {
        triggerJavaIllegalStateException(env, "incorrect false positive rate");
        return nullptr;
    }

    CBloomFilter bloom(std::max((size_t)capacity, len), falsePosRate, tweak, flags, maxSize);

    for (size_t i = 0; i < len; i++)
    {
        jobject obj = env->GetObjectArrayElement(arg, i);
        if (!env->IsInstanceOf(obj, byteArrayClass))
        {
            triggerJavaIllegalStateException(env, "incorrect element data type (must be ByteArray)");
            return nullptr;
        }
        jbyteArray elem = (jbyteArray)obj;
        jbyte *elemData = env->GetByteArrayElements(elem, 0);
        if (elemData == NULL)
        {
            triggerJavaIllegalStateException(env, "incorrect element data type (must be ByteArray)");
            return nullptr;
        }
        size_t elemLen = env->GetArrayLength(elem);
        bloom.insert(std::vector<unsigned char>(elemData, elemData + elemLen));
        env->ReleaseByteArrayElements(elem, elemData, 0);
    }

    CDataStream serializer(SER_NETWORK, PROTOCOL_VERSION);
    serializer << bloom;
    __android_log_print(ANDROID_LOG_INFO, APPNAME, "Bloom size: %d Bloom serialized size: %d numAddrs: %d\n",
        (unsigned int)bloom.vDataSize(), (unsigned int)serializer.size(), (unsigned int)len);
    jbyteArray ret = env->NewByteArray(serializer.size());
    jbyte *retData = env->GetByteArrayElements(ret, 0);

    if (!retData)
        return ret; // failed
    memcpy(retData, serializer.data(), serializer.size());

    env->ReleaseByteArrayElements(ret, retData, 0);
    return ret;
}

/** Get work from nbits */
extern "C" JNIEXPORT jbyteArray JNICALL
Java_bitcoinunlimited_libbitcoincash_Blockchain_getWorkFromDifficultyBits(JNIEnv *env, jobject ths, jlong nBits)
{
    arith_uint256 result = GetWorkForDifficultyBits((uint32_t)nBits);
    return encodeUint256(env, result);
}

/** Given a private key, return its corresponding public key */
extern "C" JNIEXPORT jbyteArray JNICALL Java_bitcoinunlimited_libbitcoincash_PayDestination_GetPubKey(JNIEnv *env,
    jobject ths,
    jbyteArray arg)
{
    size_t len = env->GetArrayLength(arg);
    jbyte *data = env->GetByteArrayElements(arg, nullptr);

    if (len != 32)
    {
        std::stringstream err;
        err << "GetPubKey: Incorrect length for argument 'secret'. "
            << "Expected 32, got " << len << ".";
        triggerJavaIllegalStateException(env, err.str().c_str());
        return nullptr;
    }
    assert(len == 32);

    CKey k = LoadKey((unsigned char *)data);
    CPubKey pub = k.GetPubKey();
    jbyteArray bArray = env->NewByteArray(pub.size());
    jbyte *dest = env->GetByteArrayElements(bArray, 0);
    memcpy(dest, pub.begin(), pub.size());

    env->ReleaseByteArrayElements(arg, data, 0);
    env->ReleaseByteArrayElements(bArray, dest, 0);
    return bArray;
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_bitcoinunlimited_libbitcoincash_Key_signDataUsingSchnorr(JNIEnv *env,
    jobject ths,
    jbyteArray message,
    jbyteArray secret)
{
    ByteArrayAccessor data(env, message);
    ByteArrayAccessor privkey(env, secret);
    if (privkey.size != 32)
    {
        std::stringstream err;
        err << "signDataUsingSchnorr: Incorrect length for argument 'secret'. "
            << "Expected 32, got " << privkey.size << ".";
        triggerJavaIllegalStateException(env, err.str().c_str());
        return nullptr;
    }

    if (data.size == 0)
    {
        triggerJavaIllegalStateException(env, "signDataUsingSchnorr: Cannot sign data of 0 length.");
        return nullptr;
    }
    if (data.size != 32)
    {
        triggerJavaIllegalStateException(env, "signDataUsingSchnorr: Must sign a 32 byte hash.");
        return nullptr;
    }

    unsigned char result[MAX_SIG_LEN];
    uint32_t resultLen = SignHashSchnorr(data.data, privkey.data, result, MAX_SIG_LEN);

    if (resultLen == 0)
    {
        triggerJavaIllegalStateException(env, "signDataUsingSchnorr: Failed to sign data.");
        return nullptr;
    }
    return makeJByteArray(env, result, resultLen);
}


extern "C" JNIEXPORT jstring JNICALL Java_bitcoinunlimited_libbitcoincash_PayAddress_EncodeCashAddr(JNIEnv *env,
    jobject ths,
    jbyte chainSelector,
    jbyte typ,
    jbyteArray arg)
{
    size_t len = env->GetArrayLength(arg);
    if (len != 20)
    {
        triggerJavaIllegalStateException(env, "bad address argument length");
        return env->NewStringUTF("bad address argument length");
    }
    jbyte *data = env->GetByteArrayElements(arg, 0);

    uint160 tmp((const uint8_t *)data);

    CTxDestination dst = CNoDestination();
    if (typ == PayAddressTypeP2PKH)
    {
        dst = CKeyID(tmp);
    }
    else if (typ == PayAddressTypeP2SH)
    {
        dst = CScriptID(tmp);
    }
    else
    {
        triggerJavaIllegalStateException(env, "Address type cannot be encoded to cashaddr");
        return nullptr;
    }


    env->ReleaseByteArrayElements(arg, data, 0);

    const CChainParams *cp = GetChainParams((ChainSelector)chainSelector);
    if (cp == nullptr)
    {
        triggerJavaIllegalStateException(env, "Unknown blockchain selection");
        return nullptr;
    }
    std::string addrAsStr(EncodeCashAddr(dst, *cp));
    return env->NewStringUTF(addrAsStr.c_str());
}

class PubkeyExtractor
{
protected:
    const CChainParams &params;
    jbyte *dest;

public:
    PubkeyExtractor(jbyte *destination, const CChainParams &p) : params(p), dest(destination) {}
    void operator()(const CKeyID &id) const
    {
        dest[0] = PayAddressTypeP2PKH;
        memcpy(dest + 1, id.begin(), 20); // pubkey is 20 bytes
    }
    void operator()(const CScriptID &id) const
    {
        dest[0] = PayAddressTypeP2SH;
        memcpy(dest + 1, id.begin(), 20); // pubkey is 20 bytes
    }
    void operator()(const CNoDestination &) const
    {
        memset(dest, 0, 21); // not a good address
        dest[0] = PayAddressTypeNONE;
    }
    void operator()(const ScriptTemplateDestination &id) const
    {
        memset(dest, 0, 21); // TODO extract pubkey from known types?
        // TODO if (its equal to p2pkt (pay-to-pubkey-template)) dest[0] = 5; else
        dest[0] = PayAddressTypeTEMPLATE;
    }
};

extern "C" JNIEXPORT jstring JNICALL Java_bitcoinunlimited_libbitcoincash_GroupId_ToAddr(JNIEnv *env,
    jobject ths,
    jbyte chainSelector,
    jbyteArray arg)
{
    size_t len = env->GetArrayLength(arg);
    if (len < 32)
    {
        triggerJavaIllegalStateException(env, "bad address argument length");
        return env->NewStringUTF("bad address argument length");
    }
    jbyte *data = env->GetByteArrayElements(arg, 0);

    CGroupTokenID grp((uint8_t *)data, len);

    env->ReleaseByteArrayElements(arg, data, 0);

    const CChainParams *cp = GetChainParams((ChainSelector)chainSelector);
    if (cp == nullptr)
    {
        triggerJavaIllegalStateException(env, "Unknown blockchain selection");
        return nullptr;
    }
    std::string addrAsStr(EncodeGroupToken(grp));
    return env->NewStringUTF(addrAsStr.c_str());
}


extern "C" JNIEXPORT jbyteArray JNICALL Java_bitcoinunlimited_libbitcoincash_GroupId_FromAddr(JNIEnv *env,
    jobject ths,
    jbyte chainSelector,
    jstring addrstr)
{
    const CChainParams *cp = GetChainParams((ChainSelector)chainSelector);
    if (cp == nullptr)
    {
        triggerJavaIllegalStateException(env, "Unknown blockchain selection");
        return nullptr;
    }
    auto addr = toString(env, addrstr);
    CGroupTokenID gid = DecodeGroupToken(addr, *cp);
    size_t size = gid.bytes().size();
    if (size < 32) // min group id size
    {
        triggerJavaIllegalStateException(env, "Address is not a group");
        return nullptr;
    }

    jbyteArray bArray = env->NewByteArray(size);
    jbyte *data = env->GetByteArrayElements(bArray, 0);
    memcpy((uint8_t *)data, &gid.bytes().front(), size);
    env->ReleaseByteArrayElements(bArray, data, 0);
    return bArray;
}


extern "C" JNIEXPORT jbyteArray JNICALL Java_bitcoinunlimited_libbitcoincash_PayAddress_DecodeCashAddr(JNIEnv *env,
    jobject ths,
    jbyte chainSelector,
    jstring addrstr)
{
    const CChainParams *cp = GetChainParams((ChainSelector)chainSelector);
    if (cp == nullptr)
    {
        triggerJavaIllegalStateException(env, "Unknown blockchain selection");
        return nullptr;
    }

    CTxDestination dst = DecodeCashAddr(toString(env, addrstr), *cp);

    jbyteArray bArray = env->NewByteArray(21);
    jbyte *data = env->GetByteArrayElements(bArray, 0);
    std::visit(PubkeyExtractor(data, *cp), dst);
    env->ReleaseByteArrayElements(bArray, data, 0);
    return bArray;
}

// many of the args are long so that the hardened selectors (i.e. 0x80000000) are not negative
extern "C" JNIEXPORT jbyteArray JNICALL Java_bitcoinunlimited_libbitcoincash_AddressDerivationKey_Hd44DeriveChildKey(
    JNIEnv *env,
    jobject ths,
    jbyteArray masterSecretBytes,
    jlong purpose,
    jlong coinType,
    jlong account,
    jint change,
    jint index)
{
    size_t mslen = env->GetArrayLength(masterSecretBytes);
    if ((mslen < 16) || (mslen > 64))
    {
        triggerJavaIllegalStateException(env, "key derivation failure -- master secret is incorrect length");
        return nullptr;
    }

    jbyte *msdata = env->GetByteArrayElements(masterSecretBytes, 0);

    CKey secret;
    Hd44DeriveChildKey((unsigned char *)msdata, mslen, purpose, coinType, account, change, index, secret, nullptr);

    jbyteArray bArray = env->NewByteArray(32);
    jbyte *data = env->GetByteArrayElements(bArray, 0);
    if (secret.size() != 32)
    {
        triggerJavaIllegalStateException(env, "key derivation failure -- derived secret is incorrect length");
        return nullptr;
    }
    memcpy(data, secret.begin(), 32);
    env->ReleaseByteArrayElements(bArray, data, 0);
    return bArray;
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_bitcoinunlimited_libbitcoincash_Hash_sha256(JNIEnv *env,
    jobject ths,
    jbyteArray arg)
{
    size_t len = env->GetArrayLength(arg);
    jbyte *data = env->GetByteArrayElements(arg, 0);

    jbyteArray bArray = env->NewByteArray(32);
    jbyte *dest = env->GetByteArrayElements(bArray, 0);
    sha256((unsigned char *)data, len, (unsigned char *)dest);

    env->ReleaseByteArrayElements(arg, data, 0);
    env->ReleaseByteArrayElements(bArray, dest, 0);
    return bArray;
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_bitcoinunlimited_libbitcoincash_Hash_hash256(JNIEnv *env,
    jobject ths,
    jbyteArray arg)
{
    size_t len = env->GetArrayLength(arg);
    jbyte *data = env->GetByteArrayElements(arg, 0);

    jbyteArray bArray = env->NewByteArray(32);
    jbyte *dest = env->GetByteArrayElements(bArray, 0);
    hash256((unsigned char *)data, len, (unsigned char *)dest);

    env->ReleaseByteArrayElements(arg, data, 0);
    env->ReleaseByteArrayElements(bArray, dest, 0);
    return bArray;
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_bitcoinunlimited_libbitcoincash_Hash_hash160(JNIEnv *env,
    jobject ths,
    jbyteArray arg)
{
    size_t len = env->GetArrayLength(arg);
    jbyte *data = env->GetByteArrayElements(arg, 0);

    jbyteArray bArray = env->NewByteArray(20);
    jbyte *dest = env->GetByteArrayElements(bArray, 0);
    hash160((unsigned char *)data, len, (unsigned char *)dest);

    env->ReleaseByteArrayElements(arg, data, 0);
    env->ReleaseByteArrayElements(bArray, dest, 0);
    return bArray;
}

class CDecodablePartialMerkleTree : public CPartialMerkleTree
{
public:
    std::vector<uint256> &accessHashes() { return vHash; }
    CDecodablePartialMerkleTree(unsigned int ntx, char *bitField, int bitFieldLen)
    {
        nTransactions = ntx;
        vBits.resize(bitFieldLen * 8);
        for (unsigned int p = 0; p < vBits.size(); p++)
            vBits[p] = (bitField[p / 8] & (1 << (p % 8))) != 0;
        fBad = false;
    }
};

extern "C" JNIEXPORT jobjectArray JNICALL Java_bitcoinunlimited_libbitcoincash_MerkleBlock_Extract(JNIEnv *env,
    jobject ths,
    jint numTxes,
    jbyteArray merkleProofPath,
    jobjectArray hashArray)
{
    const unsigned int HASH_LEN = 32;
    size_t hashArrayLen = env->GetArrayLength(hashArray);

    jbyte *mppData = env->GetByteArrayElements(merkleProofPath, 0);
    size_t mppLen = env->GetArrayLength(merkleProofPath);
    CDecodablePartialMerkleTree tree(numTxes, (char *)mppData, mppLen);
    env->ReleaseByteArrayElements(merkleProofPath, mppData, 0);

    // Copy the hashes out of the java wrapper objects into the PartialMerkleTree
    auto &hashes = tree.accessHashes();
    hashes.resize(hashArrayLen);
    for (size_t i = 0; i < hashArrayLen; i++)
    {
        jbyteArray elem = (jbyteArray)env->GetObjectArrayElement(hashArray, i);
        jbyte *elemData = env->GetByteArrayElements(elem, 0);
        size_t elemLen = env->GetArrayLength(elem);
        if (elemLen != HASH_LEN)
        {
            triggerJavaIllegalStateException(env, "invalid hash: bad length");
            return nullptr;
        }
        hashes[i] = uint256((unsigned char *)elemData);
        env->ReleaseByteArrayElements(elem, elemData, 0);
    }

    std::vector<uint256> matches;
    std::vector<unsigned int> matchIndexes;
    uint256 merkleRoot = tree.ExtractMatches(matches, matchIndexes);

    jclass elementClass = env->GetObjectClass(merkleProofPath); // get the class of a jbyteArray
    jobjectArray ret = env->NewObjectArray(matches.size() + 1, elementClass, nullptr);

    // Put the merkle root in the first slot
    {
        jbyteArray bArray = env->NewByteArray(HASH_LEN);
        jbyte *dest = env->GetByteArrayElements(bArray, 0);
        memcpy(dest, merkleRoot.begin(), HASH_LEN);
        env->ReleaseByteArrayElements(bArray, dest, 0);
        env->SetObjectArrayElement(ret, 0, bArray);
    }

    // Fill the rest with transactions hashes
    for (size_t i = 0; i < matches.size(); i++)
    {
        jbyteArray bArray = env->NewByteArray(HASH_LEN);
        jbyte *dest = env->GetByteArrayElements(bArray, 0);
        memcpy(dest, matches[i].begin(), HASH_LEN);
        env->ReleaseByteArrayElements(bArray, dest, 0);
        env->SetObjectArrayElement(ret, i + 1, bArray);
    }
    return ret;
}

extern "C" JNIEXPORT jstring JNICALL Java_bitcoinunlimited_libbitcoincash_Initialize_LibBitcoinCash(JNIEnv *env,
    jobject ths,
    jbyte chainSelector)
{
    javaEnv = env;

    cashlibParams = GetChainParams((ChainSelector)chainSelector);
    if (cashlibParams == nullptr)
    {
        triggerJavaIllegalStateException(env, "unknown blockchain selection");
    }

#ifdef ANDROID
    // initialize the env globals and hook up the random number generator
    jclass c = env->FindClass("bitcoinunlimited/libbitcoincash/Initialize");
    if (c == nullptr)
    {
        __android_log_print(ANDROID_LOG_VERBOSE, APPNAME, "class not found\n");
    }
    else
    {
        secRandomClass = reinterpret_cast<jclass>(env->NewGlobalRef(c));
        //__android_log_print(ANDROID_LOG_VERBOSE, APPNAME, "class found: %x", secRandomClass);
        // Get the method that you want to call
        secRandom = env->GetStaticMethodID(c, "SecRandom", "([B)V");
        //__android_log_print(ANDROID_LOG_VERBOSE, APPNAME, "method ID: %x", secRandom);
    }
#endif

    // must be below the random number generator hookup
    checkSigInit();

    return env->NewStringUTF("");
}

#ifdef ANDROID
void RandAddSeedPerfmon()
{
    // Android random # generator is already seeded so nothing to do
}

// Implement in Android by calling into the java SecureRandom implementation.
// You must provide this Java API
SLAPI int RandomBytes(unsigned char *buf, int num)
{
    jbyteArray bArray = javaEnv->NewByteArray(num);
    javaEnv->CallStaticVoidMethod(secRandomClass, secRandom, bArray);
    javaEnv->GetByteArrayRegion(bArray, 0, num, (jbyte *)buf);
    javaEnv->DeleteLocalRef(bArray);
    return num;
}
// Implement APIs normally provided by random.cpp calling openssl
void GetRandBytes(unsigned char *buf, int num) { RandomBytes(buf, num); }
void GetStrongRandBytes(unsigned char *buf, int num) { RandomBytes(buf, num); }
#define JAVA_ANDROID

#endif
#endif

#ifndef JAVA_ANDROID
/** Return random bytes from cryptographically acceptable random sources */
SLAPI int RandomBytes(unsigned char *buf, int num)
{
    if (RAND_bytes(buf, num) != 1)
    {
        memset(buf, 0, num);
        return 0;
    }
    return num;
}
#endif
