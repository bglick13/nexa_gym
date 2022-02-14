// Copyright (c) 2015-2019 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/* clang-format off */
// must be first for windows
#include "compat.h"

/* clang-format on */
#include "base58.h"
#include "primitives/transaction.h"
#include "script/sighashtype.h"
#include "script/sign.h"
#include "stdio.h"
#include "streams.h"
#include "uint256.h"
#include "utilstrencodings.h"

#include <boost/algorithm/string.hpp>
#include <string>
#include <vector>

#ifdef ANDROID // log sighash calculations
#include <android/log.h>
#define p(...) __android_log_print(ANDROID_LOG_DEBUG, "bu.sig", __VA_ARGS__)
#else
#define p(...)
// tinyformat::format(std::cout, __VA_ARGS__)
#endif

const SigHashType defaultSigHashType(SIGHASH_ALL | SIGHASH_FORKID);

uint256 GetPrevoutHash(const CTransaction &txTo)
{
    CHashWriter ss(SER_GETHASH, 0);
    for (unsigned int n = 0; n < txTo.vin.size(); n++)
    {
        ss << txTo.vin[n].prevout;
    }
    return ss.GetHash();
}

uint256 GetInputAmountHash(const CTransaction &txTo)
{
    CHashWriter ss(SER_GETHASH, 0);
    for (unsigned int n = 0; n < txTo.vin.size(); n++)
    {
        ss << txTo.vin[n].amount;
    }
    return ss.GetHash();
}

uint256 GetSequenceHash(const CTransaction &txTo)
{
    CHashWriter ss(SER_GETHASH, 0);
    for (unsigned int n = 0; n < txTo.vin.size(); n++)
    {
        ss << txTo.vin[n].nSequence;
    }
    return ss.GetHash();
}

uint256 GetOutputsHash(const CTransaction &txTo)
{
    CHashWriter ss(SER_GETHASH, 0);
    for (unsigned int n = 0; n < txTo.vout.size(); n++)
    {
        ss << txTo.vout[n];
    }
    return ss.GetHash();
}

/**
 * Wrapper that serializes like CTransaction, but with the modifications
 *  required for the signature hash done in-place
 */
class CTransactionSignatureSerializer
{
private:
    const CTransaction &txTo; //! reference to the spending transaction (the one being serialized)
    const CScript &scriptCode; //! output script being consumed
    const unsigned int nIn; //! input index of txTo being signed
    const bool fAnyoneCanPay; //! whether the hashtype has the SIGHASH_ANYONECANPAY flag set
    const bool fHashSingle; //! whether the hashtype is SIGHASH_SINGLE
    const bool fHashNone; //! whether the hashtype is SIGHASH_NONE

public:
    CTransactionSignatureSerializer(const CTransaction &txToIn,
        const CScript &scriptCodeIn,
        unsigned int nInIn,
        SigHashType hashTypeIn)
        : txTo(txToIn), scriptCode(scriptCodeIn), nIn(nInIn), fAnyoneCanPay(hashTypeIn.hasAnyoneCanPay()),
          fHashSingle(hashTypeIn.hasSingle()), fHashNone(hashTypeIn.hasNone())
    {
    }

    /** Serialize the passed scriptCode, skipping OP_CODESEPARATORs */
    template <typename S>
    void SerializeScriptCode(S &s) const
    {
        CScript::const_iterator it = scriptCode.begin();
        CScript::const_iterator itBegin = it;
        opcodetype opcode;
        unsigned int nCodeSeparators = 0;
        while (scriptCode.GetOp(it, opcode))
        {
            if (opcode == OP_CODESEPARATOR)
                nCodeSeparators++;
        }
        ::WriteCompactSize(s, scriptCode.size() - nCodeSeparators);
        it = itBegin;
        while (scriptCode.GetOp(it, opcode))
        {
            if (opcode == OP_CODESEPARATOR)
            {
                s.write((char *)&itBegin[0], it - itBegin - 1);
                itBegin = it;
            }
        }
        if (itBegin != scriptCode.end())
            s.write((char *)&itBegin[0], it - itBegin);
    }

    /** Serialize an input of txTo */
    template <typename S>
    void SerializeInput(S &s, unsigned int nInput) const
    {
        // In case of SIGHASH_ANYONECANPAY, only the input being signed is serialized
        if (fAnyoneCanPay)
            nInput = nIn;
        // Serialize the prevout
        ::Serialize(s, txTo.vin[nInput].prevout);
        // Serialize the script
        if (nInput != nIn)
            // Blank out other inputs' signatures
            ::Serialize(s, CScriptBase());
        else
            SerializeScriptCode(s);
        // Serialize the nSequence
        if (nInput != nIn && (fHashSingle || fHashNone))
            // let the others update at will
            ::Serialize(s, (int)0);
        else
            ::Serialize(s, txTo.vin[nInput].nSequence);
        ::Serialize(s, txTo.vin[nInput].amount);
    }

    /** Serialize an output of txTo */
    template <typename S>
    void SerializeOutput(S &s, unsigned int nOutput) const
    {
        if (fHashSingle && nOutput != nIn)
            // Do not lock-in the txout payee at other indices as txin
            ::Serialize(s, CTxOut());
        else
            ::Serialize(s, txTo.vout[nOutput]);
    }

    /** Serialize txTo */
    template <typename S>
    void Serialize(S &s) const
    {
        // Serialize nVersion
        ::Serialize(s, txTo.nVersion);
        // Serialize vin
        unsigned int nInputs = fAnyoneCanPay ? 1 : txTo.vin.size();
        ::WriteCompactSize(s, nInputs);
        for (unsigned int nInput = 0; nInput < nInputs; nInput++)
            SerializeInput(s, nInput);
        // Serialize vout
        unsigned int nOutputs = fHashNone ? 0 : (fHashSingle ? nIn + 1 : txTo.vout.size());
        ::WriteCompactSize(s, nOutputs);
        for (unsigned int nOutput = 0; nOutput < nOutputs; nOutput++)
            SerializeOutput(s, nOutput);
        // Serialize nLockTime
        ::Serialize(s, txTo.nLockTime);
    }
};


// WARNING: Never use this to signal errors in a signature hash function. This is here solely for legacy reasons!
const uint256 SIGNATURE_HASH_ERROR(uint256S("0000000000000000000000000000000000000000000000000000000000000001"));

uint256 SignatureHashBitcoin(const CScript &scriptCode,
    const CTransaction &txTo,
    unsigned int nIn,
    const SigHashType &sigHashType,
    size_t *nHashedOut)
{
    if (nIn >= txTo.vin.size())
    {
        //  nIn out of range
        // IMPORTANT NOTICE:
        // Returning one from SignatureHash..() to signal error conditions is a kludge that
        // is also breaking the ECDSA assumption that only cryptographic hashes are signed. The special value
        // returned here is, however, due to further omissions in CheckSig, part of the pre-BCH
        // consensus rule set and needs to be left as-is.
        // See also: https://lists.linuxfoundation.org/pipermail/bitcoin-dev/2014-November/006878.html
        return SIGNATURE_HASH_ERROR;
    }

    // Check for invalid use of SIGHASH_SINGLE
    uint8_t nHashType = sigHashType.btcSigHashType();
    if ((nHashType & 0x1f) == SIGHASH_SINGLE)
    {
        if (nIn >= txTo.vout.size())
        {
            //  nOut out of range
            // IMPORTANT NOTICE:
            // Returning one from SignatureHash..() to signal error conditions is a kludge that
            // is also breaking the ECDSA assumption that only cryptographic hashes are signed. The special value
            // returned here is, however, due to further omissions in CheckSig, part of the pre-BCH
            // consensus rule set and needs to be left as-is.
            // See also: https://lists.linuxfoundation.org/pipermail/bitcoin-dev/2014-November/006878.html
            return SIGNATURE_HASH_ERROR;
        }
    }

    // Wrapper to serialize only the necessary parts of the transaction being signed
    CTransactionSignatureSerializer txTmp(txTo, scriptCode, nIn, sigHashType);

    // Serialize and hash
    CHashWriter ss(SER_GETHASH, 0);
    ss << txTmp << sigHashType.getRawSigHashType();
    if (nHashedOut != nullptr)
        *nHashedOut = ss.GetNumBytesHashed();
    return ss.GetHash();
}

// ONLY to be called with SIGHASH_FORKID set in nHashType!
uint256 SignatureHashBitcoinCash(const CScript &scriptCode,
    const CTransaction &txTo,
    unsigned int nIn,
    const SigHashType &sigHashType,
    const CAmount &amount,
    size_t *nHashedOut)
{
    uint256 hashPrevouts;
    uint256 hashSequence;
    uint256 hashInputAmounts;
    uint256 hashOutputs;

    uint8_t nHashType = sigHashType.bchSigHashType();

    p("Signature hash calculation with type: 0x%x\n", nHashType);
    if (!(nHashType & SIGHASH_ANYONECANPAY))
    {
        hashPrevouts = GetPrevoutHash(txTo);
        p("Hashing prevouts to: %s\n", hashPrevouts.GetHex().c_str());
        hashInputAmounts = GetInputAmountHash(txTo);
        p("Hashing input amounts to: %s\n", hashInputAmounts.GetHex().c_str());
    }

    /* gets the hash of the sequence numbers of every input */
    if (!(nHashType & SIGHASH_ANYONECANPAY) && (nHashType & 0x1f) != SIGHASH_SINGLE &&
        (nHashType & 0x1f) != SIGHASH_NONE)
    {
        hashSequence = GetSequenceHash(txTo);
        p("Hashing input sequence numbers to: %s\n", hashSequence.GetHex().c_str());
    }

    /* gets the hash of the serialization of every output */
    if ((nHashType & 0x1f) != SIGHASH_SINGLE && (nHashType & 0x1f) != SIGHASH_NONE)
    {
        hashOutputs = GetOutputsHash(txTo);
        p("Hashing every output to: %s\n", hashOutputs.GetHex().c_str());
    }
    /* Or just serialize the output that corresponds to this input */
    else if ((nHashType & 0x1f) == SIGHASH_SINGLE && nIn < txTo.vout.size())
    {
        CHashWriter ss(SER_GETHASH, 0);
        ss << txTo.vout[nIn];
        hashOutputs = ss.GetHash();
        p("Hashing just output %d to: %s\n", nIn, hashOutputs.GetHex().c_str());
    }

    CHashWriter ss(SER_GETHASH, 0);
    // Version
    ss << txTo.nVersion;
    // Input prevouts/nSequence (none/all, depending on flags)
    ss << hashPrevouts;
    ss << hashInputAmounts;
    ss << hashSequence;
    // The input being signed (replacing the scriptSig with scriptCode +
    // amount). The prevout may already be contained in hashPrevout, and the
    // nSequence may already be contain in hashSequence.
    ss << txTo.vin[nIn].prevout;
    ss << static_cast<const CScriptBase &>(scriptCode);
    p("ScriptCode: %s\n", scriptCode.GetHex().c_str());
    ss << txTo.vin[nIn].amount;
    p("Amount: %ld\n", (long int)txTo.vin[nIn].amount);
    ss << txTo.vin[nIn].nSequence;
    p("This input sequence: %d\n", txTo.vin[nIn].nSequence);
    // Outputs (none/one/all, depending on flags)
    ss << hashOutputs;
    // Locktime
    ss << txTo.nLockTime;
    p("Locktime: %d\n", txTo.nLockTime);
    // Sighash type
    ss << sigHashType;
    p("sigHashType: %x\n", sigHashType.getRawSigHashType());

    p("Num bytes hashed: %d\n", ss.GetNumBytesHashed());
    uint256 sighash = ss.GetHash();
    p("Final sighash is: %s\n", sighash.GetHex().c_str());
    return sighash;
}


uint256 SignatureHash(const CScript &scriptCode,
    const CTransaction &txTo,
    unsigned int nIn,
    const SigHashType &sigHashType,
    const CAmount &amount,
    size_t *nHashedOut)
{
    if (sigHashType.isInvalid())
    {
        return SIGNATURE_HASH_ERROR;
    }
    if (sigHashType.isBch())
    {
        return SignatureHashBitcoinCash(scriptCode, txTo, nIn, sigHashType, amount, nHashedOut);
    }
    return SignatureHashBitcoin(scriptCode, txTo, nIn, sigHashType, nHashedOut);
}

SigHashType GetSigHashType(const std::vector<unsigned char> &vchSig)
{
    if (vchSig.size() == 0)
    {
        return SigHashType(BaseSigHashType::UNSUPPORTED);
    }

    return SigHashType(vchSig[vchSig.size() - 1]);
}

void RemoveSigHashType(std::vector<unsigned char> &vchSig)
{
    vchSig.resize(64); // Schnorr signatures are 64 bytes
}

/** Convert to a human readable representation of the sighash */
std::string SigHashType::ToString() const
{
    if (isInvalid())
        return std::string("UNSUPPORTED");
    std::string ret;
    if (hasSingle())
        ret = "SINGLE";
    else if (hasNone())
        ret = "NONE";
    else if (hasAll())
        ret = "ALL";
    else
        return std::string("UNSUPPORTED");

    if (isBch())
        ret += "|FORKID";
    if (hasAnyoneCanPay())
        ret += "|ANYONECANPAY";
    return ret;
}


SigHashType &SigHashType::from(const std::string &flagStr)
{
    std::vector<std::string> strings;
    std::istringstream ss(flagStr);
    std::string s;
    while (getline(ss, s, '|'))
    {
        boost::trim(s);
        if (s == "ALL")
            sigHash = SIGHASH_ALL;
        else if (s == "NONE")
            sigHash = SIGHASH_NONE;
        else if (s == "SINGLE")
            sigHash = SIGHASH_SINGLE;
        else if (s == "ANYONECANPAY")
            sigHash |= SIGHASH_ANYONECANPAY;
        else if (s == "FORKID")
            sigHash |= SIGHASH_FORKID;
        else if (s == "NOFORKID")
            sigHash &= ~SIGHASH_FORKID;
        else // Abort if incorrect string
        {
            sigHash = (uint32_t)BaseSigHashType::UNSUPPORTED;
            return *this;
        }
    }
    return *this;
}
