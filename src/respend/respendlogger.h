// Copyright (c) 2018 The Bitcoin developers
// Copyright (c) 2018-2020 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef NEXA_RESPEND_RESPENDLOGGER_H
#define NEXA_RESPEND_RESPENDLOGGER_H

#include "respend/respendaction.h"
#include <string>

namespace respend
{
class RespendLogger : public RespendAction
{
public:
    RespendLogger();

    bool AddOutpointConflict(const COutPoint &,
        const uint256 hash,
        const CTransactionRef pRespendTx,
        bool seen,
        bool isEquivalent) override;

    virtual bool IsInteresting() const override;

    void Trigger(CTxMemPool &pool) override;

    void SetValid(bool v) override { valid = v ? "yes" : "no"; }

private:
    std::string orig;
    std::string respend;
    bool equivalent;
    std::string valid;
    bool newConflict; // TX has at least 1 output that's not respent earlier
};

} // namespace respend

#endif
