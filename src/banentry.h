// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2015-2020 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef NEXA_BANENTRY_H
#define NEXA_BANENTRY_H

// NOTE: netaddress.h includes serialize.h which is required for serialization macros
#include "netaddress.h" // for CSubNet

typedef enum BanReason
{
    BanReasonUnknown = 0,
    BanReasonNodeMisbehaving = 1,
    BanReasonManuallyAdded = 2,
    BanReasonTooManyEvictions = 3,
    BanReasonTooManyConnectionAttempts = 4,
    BanReasonInvalidMessageStart = 5,
    BanReasonInvalidInventory = 6,
    BanReasonInvalidPeer = 7
} BanReason;

class CBanEntry
{
public:
    static const int CURRENT_VERSION = 1;
    int nVersion;
    int64_t nCreateTime;
    int64_t nBanUntil;
    uint8_t banReason;
    std::string userAgent;

    CBanEntry();
    CBanEntry(int64_t nCreateTimeIn);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream &s, Operation ser_action)
    {
        READWRITE(this->nVersion);
        READWRITE(nCreateTime);
        READWRITE(nBanUntil);
        READWRITE(banReason);
        READWRITE(userAgent);
    }

    void SetNull();

    std::string banReasonToString();
};

typedef std::map<CSubNet, CBanEntry> banmap_t;

#endif // NEXA_BANENTRY_H
