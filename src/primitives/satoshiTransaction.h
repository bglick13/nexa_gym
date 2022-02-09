// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2015-2019 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PRIMITIVES_SATOSHI_TRANSACTION_H
#define BITCOIN_PRIMITIVES_SATOSHI_TRANSACTION_H

#include "amount.h"
#include "script/script.h"
#include "serialize.h"
#include "tweak.h"
#include "uint256.h"

#include <atomic>
#include <memory>
extern CTweak<unsigned int> nDustThreshold;

/** An outpoint - a combination of a transaction hash and an index n into its vout */
class SatoshiOutPoint
{
public:
    uint256 hash;
    uint32_t n;

    SatoshiOutPoint() { SetNull(); }
    SatoshiOutPoint(uint256 hashIn, uint32_t nIn)
    {
        hash = hashIn;
        n = nIn;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream &s, Operation ser_action)
    {
        READWRITE(hash);
        READWRITE(n);
    }

    void SetNull()
    {
        hash.SetNull();
        n = (uint32_t)-1;
    }
    bool IsNull() const { return (hash.IsNull() && n == (uint32_t)-1); }
    friend bool operator<(const SatoshiOutPoint &a, const SatoshiOutPoint &b)
    {
        return (a.hash < b.hash || (a.hash == b.hash && a.n < b.n));
    }

    friend bool operator==(const SatoshiOutPoint &a, const SatoshiOutPoint &b) { return (a.hash == b.hash && a.n == b.n); }
    friend bool operator!=(const SatoshiOutPoint &a, const SatoshiOutPoint &b) { return !(a == b); }
    std::string ToString() const;
};


/** An input of a transaction.  It contains the location of the previous
 * transaction's output that it claims and a signature that matches the
 * output's public key.
 */
class SatoshiTxIn
{
public:
    SatoshiOutPoint prevout;
    CScript scriptSig;
    uint32_t nSequence;

    /* Setting nSequence to this value for every input in a transaction
     * disables nLockTime. */
    static const uint32_t SEQUENCE_FINAL = 0xffffffff;

    /* Below flags apply in the context of BIP 68*/
    /* If this flag set, CTxIn::nSequence is NOT interpreted as a
     * relative lock-time. */
    static const uint32_t SEQUENCE_LOCKTIME_DISABLE_FLAG = (1U << 31);

    /* If CTxIn::nSequence encodes a relative lock-time and this flag
     * is set, the relative lock-time has units of 512 seconds,
     * otherwise it specifies blocks with a granularity of 1. */
    static const uint32_t SEQUENCE_LOCKTIME_TYPE_FLAG = (1 << 22);

    /* If CTxIn::nSequence encodes a relative lock-time, this mask is
     * applied to extract that lock-time from the sequence field. */
    static const uint32_t SEQUENCE_LOCKTIME_MASK = 0x0000ffff;

    /* In order to use the same number of bits to encode roughly the
     * same wall-clock duration, and because blocks are naturally
     * limited to occur every 600s on average, the minimum granularity
     * for time-based relative lock-time is fixed at 512 seconds.
     * Converting from CTxIn::nSequence to seconds is performed by
     * multiplying by 512 = 2^9, or equivalently shifting up by
     * 9 bits. */
    static const int SEQUENCE_LOCKTIME_GRANULARITY = 9;

    SatoshiTxIn() { nSequence = SEQUENCE_FINAL; }
    explicit SatoshiTxIn(SatoshiOutPoint prevoutIn, CScript scriptSigIn = CScript(), uint32_t nSequenceIn = SEQUENCE_FINAL);
    SatoshiTxIn(uint256 hashPrevTx, uint32_t nOut, CScript scriptSigIn = CScript(), uint32_t nSequenceIn = SEQUENCE_FINAL);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream &s, Operation ser_action)
    {
        READWRITE(prevout);
        READWRITE(*(CScriptBase *)(&scriptSig));
        READWRITE(nSequence);
    }

    friend bool operator==(const SatoshiTxIn &a, const SatoshiTxIn &b)
    {
        return (a.prevout == b.prevout && a.scriptSig == b.scriptSig && a.nSequence == b.nSequence);
    }

    friend bool operator!=(const SatoshiTxIn &a, const SatoshiTxIn &b) { return !(a == b); }
    std::string ToString() const;
};


/** An output of a transaction.  It contains the public key that the next input
 * must be able to sign with to claim it.
 */
class SatoshiTxOut
{
public:
    CAmount nValue;
    CScript scriptPubKey;

    SatoshiTxOut() { SetNull(); }
    SatoshiTxOut(const CAmount &nValueIn, CScript scriptPubKeyIn);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream &s, Operation ser_action)
    {
        READWRITE(nValue);
        READWRITE(*(CScriptBase *)(&scriptPubKey));
    }

    void SetNull()
    {
        nValue = -1;
        scriptPubKey.clear();
    }

    bool IsNull() const { return (nValue == -1); }
    uint256 GetHash() const;

    CAmount GetDustThreshold() const
    {
        if (scriptPubKey.IsUnspendable())
            return (CAmount)0;

        return (CAmount)nDustThreshold.Value();
    }
    bool IsDust() const { return (nValue < GetDustThreshold()); }
    friend bool operator==(const SatoshiTxOut &a, const SatoshiTxOut &b)
    {
        return (a.nValue == b.nValue && a.scriptPubKey == b.scriptPubKey);
    }

    friend bool operator!=(const SatoshiTxOut &a, const SatoshiTxOut &b) { return !(a == b); }
    std::string ToString() const;
};

struct MutableSatoshiTransaction;

/** The basic transaction that is broadcasted on the network and contained in
 * blocks.  A transaction can contain multiple inputs and outputs.
 */
class SatoshiTransaction
{
private:
    /** Memory only. */
    const uint256 hash;
    void UpdateHash() const;
    mutable std::atomic<size_t> nTxSize; // Serialized transaction size in bytes.


public:
    // Default transaction version.
    static const int32_t CURRENT_VERSION = 1;

    // Changing the default transaction version requires a two step process: first
    // adapting relay policy by bumping MAX_STANDARD_VERSION, and then later date
    // bumping the default CURRENT_VERSION at which point both CURRENT_VERSION and
    // MAX_STANDARD_VERSION will be equal.
    static const int32_t MAX_STANDARD_VERSION = 2;

    // The local variables are made const to prevent unintended modification
    // without updating the cached hash value. However, SatoshiTransaction is not
    // actually immutable; deserialization and assignment are implemented,
    // and bypass the constness. This is safe, as they update the entire
    // structure, including the hash.
    const int32_t nVersion;
    const std::vector<SatoshiTxIn> vin;
    const std::vector<SatoshiTxOut> vout;
    const uint32_t nLockTime;

    /** Construct a SatoshiTransaction that qualifies as IsNull() */
    SatoshiTransaction();

    /** Convert a MutableSatoshiTransaction into a SatoshiTransaction. */
    SatoshiTransaction(const MutableSatoshiTransaction &tx);
    SatoshiTransaction(MutableSatoshiTransaction &&tx);

    SatoshiTransaction(const SatoshiTransaction &tx);
    SatoshiTransaction &operator=(const SatoshiTransaction &tx);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream &s, Operation ser_action)
    {
        READWRITE(*const_cast<int32_t *>(&this->nVersion));
        READWRITE(*const_cast<std::vector<SatoshiTxIn> *>(&vin));
        READWRITE(*const_cast<std::vector<SatoshiTxOut> *>(&vout));
        READWRITE(*const_cast<uint32_t *>(&nLockTime));
        if (ser_action.ForRead())
            UpdateHash();
    }

    template <typename Stream>
    SatoshiTransaction(deserialize_type, Stream &s) : SatoshiTransaction(MutableSatoshiTransaction(deserialize, s))
    {
    }

    bool IsNull() const { return vin.empty() && vout.empty(); }
    const uint256 &GetHash() const { return hash; }
    // True if only scriptSigs are different
    bool IsEquivalentTo(const SatoshiTransaction &tx) const;

    //* Return true if this transaction contains at least one OP_RETURN output.
    bool HasData() const;
    //* Return true if this transaction contains at least one OP_RETURN output, with the specified data ID
    // the data ID is defined as a 4 byte pushdata containing a little endian 4 byte integer.
    bool HasData(uint32_t dataID) const;

    // Return sum of txouts.
    CAmount GetValueOut() const;
    // GetValueIn() is a method on CCoinsViewCache, because
    // inputs must be known to compute value in.

    // Compute priority, given priority of inputs and (optionally) tx size
    double ComputePriority(double dPriorityInputs, unsigned int nSize = 0) const;

    // Compute modified tx size for priority calculation (optionally given tx size)
    unsigned int CalculateModifiedSize(unsigned int nSize = 0) const;

    bool IsCoinBase() const { return (vin.size() == 1 && vin[0].prevout.IsNull()); }
    friend bool operator==(const SatoshiTransaction &a, const SatoshiTransaction &b) { return a.hash == b.hash; }
    friend bool operator!=(const SatoshiTransaction &a, const SatoshiTransaction &b) { return a.hash != b.hash; }
    std::string ToString() const;

    // Return the size of the transaction in bytes.
    size_t GetTxSize() const;
    /** return this transaction as a hex string.  Useful for debugging and display */
    // std::string HexStr() const;
};


/** A mutable version of SatoshiTransaction. */
struct MutableSatoshiTransaction
{
    int32_t nVersion;
    std::vector<SatoshiTxIn> vin;
    std::vector<SatoshiTxOut> vout;
    uint32_t nLockTime;

    MutableSatoshiTransaction();
    MutableSatoshiTransaction(const SatoshiTransaction &tx);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream &s, Operation ser_action)
    {
        READWRITE(this->nVersion);
        READWRITE(vin);
        READWRITE(vout);
        READWRITE(nLockTime);
    }

    template <typename Stream>
    MutableSatoshiTransaction(deserialize_type, Stream &s)
    {
        Unserialize(s);
    }

    /** Compute the hash of this MutableSatoshiTransaction. This is computed on the
     * fly, as opposed to GetHash() in SatoshiTransaction, which uses a cached result.
     */
    uint256 GetHash() const;
};


#endif // BITCOIN_PRIMITIVES_SATOSHI_TRANSACTION_H
