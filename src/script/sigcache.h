// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2015-2019 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef NEXA_SCRIPT_SIGCACHE_H
#define NEXA_SCRIPT_SIGCACHE_H

#include "script/interpreter.h"

#include <vector>

// DoS prevention: limit cache size to 32MB (over 1000000 entries on 64-bit
// systems). Due to how we count cache size, actual memory usage is slightly
// more (~32.25 MB)
static const unsigned int DEFAULT_MAX_SIG_CACHE_SIZE = 32;

class CPubKey;

/**
 * We're hashing a nonce into the entries themselves, so we don't need extra
 * blinding in the set hash computation.
 *
 * This may exhibit platform endian dependent behavior but because these are
 * nonced hashes (random) and this state is only ever used locally it is safe.
 * All that matters is local consistency.
 */
class SignatureCacheHasher
{
public:
    template <uint8_t hash_select>
    uint32_t operator()(const uint256 &key) const
    {
        static_assert(hash_select < 8, "SignatureCacheHasher only has 8 hashes available.");
        uint32_t u;
        std::memcpy(&u, key.begin() + 4 * hash_select, 4);
        return u;
    }
};

class CachingTransactionSignatureChecker : public TransactionSignatureChecker
{
private:
    bool store;

public:
    CachingTransactionSignatureChecker() { store = true; }
    CachingTransactionSignatureChecker(const CTransaction *txToIn,
        unsigned int nInIn,
        const CAmount &amountIn,
        unsigned int flags,
        bool storeIn = true)
        : TransactionSignatureChecker(txToIn, nInIn, flags), store(storeIn)
    {
    }

    bool IsCached(const std::vector<uint8_t> &vchSig, const CPubKey &vchPubKey, const uint256 &sighash) const;

    bool VerifySignature(const std::vector<uint8_t> &vchSig,
        const CPubKey &pubkey,
        const uint256 &sighash) const override;
};

void InitSignatureCache();

#endif // NEXA_SCRIPT_SIGCACHE_H
