// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2015-2020 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef NEXA_VALIDATIONINTERFACE_H
#define NEXA_VALIDATIONINTERFACE_H

#include "primitives/transaction.h"

#include <boost/shared_ptr.hpp>
#include <boost/signals2/signal.hpp>

class CBlock;
struct CBlockLocator;
class CBlockIndex;
class CReserveScript;
class CValidationInterface;
class CValidationState;
class uint256;

typedef std::shared_ptr<const CBlock> ConstCBlockRef;

// These functions dispatch to one or all registered wallets

/** Register a wallet to receive updates from core */
void RegisterValidationInterface(CValidationInterface *pwalletIn);
/** Unregister a wallet from core */
void UnregisterValidationInterface(CValidationInterface *pwalletIn);
/** Unregister all wallets from core */
void UnregisterAllValidationInterfaces();
/** Push an updated transaction to all registered wallets, pass nullptr if block not known, pass -1 if txIdx not known
 */
void SyncWithWallets(const CTransactionRef &ptx, const ConstCBlockRef pblock, int txIdx);

class RaiiRegisterValidationInterface
{
public:
    CValidationInterface *cvi;
    RaiiRegisterValidationInterface(CValidationInterface *cviIn) : cvi(cviIn) { RegisterValidationInterface(cvi); }
    ~RaiiRegisterValidationInterface() { UnregisterValidationInterface(cvi); }
};

class CValidationInterface
{
protected:
    virtual void UpdatedBlockTip(const CBlockIndex *pindex) {}
    virtual void SyncTransaction(const CTransactionRef &ptx, const ConstCBlockRef pblock, int txIdx) {}
    virtual void SyncDoubleSpend(const CTransactionRef ptx) {}
    virtual void SetBestChain(const CBlockLocator &locator) {}
    virtual void UpdatedTransaction(const uint256 &txid) {}
    virtual void Inventory(const uint256 &hash) {}
    virtual void ResendWalletTransactions(int64_t nBestBlockTime) {}
    virtual void BlockChecked(const CBlock &, const CValidationState &) {}
    virtual void GetScriptForMining(boost::shared_ptr<CReserveScript> &) {}
    virtual void ResetRequestCount(const uint256 &hash) {}
    friend void ::RegisterValidationInterface(CValidationInterface *);
    friend void ::UnregisterValidationInterface(CValidationInterface *);
    friend void ::UnregisterAllValidationInterfaces();
};

struct CMainSignals
{
    /** Notifies listeners of updated block chain tip */
    boost::signals2::signal<void(const CBlockIndex *)> UpdatedBlockTip;
    /** Notifies listeners of updated transaction data (transaction, and optionally the block it is found in. */
    boost::signals2::signal<void(const CTransactionRef &, const ConstCBlockRef, int txIndex)> SyncTransaction;
    /** Notifies listeners of a transaction in the mempool that was double spent. */
    boost::signals2::signal<void(const CTransactionRef)> SyncDoubleSpend;
    /** Notifies listeners of an updated transaction without new data (for now: a coinbase potentially becoming
     * visible). */
    boost::signals2::signal<void(const uint256 &)> UpdatedTransaction;
    /** Notifies listeners of a new active block chain. */
    boost::signals2::signal<void(const CBlockLocator &)> SetBestChain;
    /** Notifies listeners about an inventory item being seen on the network. */
    boost::signals2::signal<void(const uint256 &)> Inventory;
    /** Tells listeners to broadcast their data. */
    boost::signals2::signal<void(int64_t nBestBlockTime)> Broadcast;
    /** Notifies listeners of a block validation result */
    boost::signals2::signal<void(const CBlock &, const CValidationState &)> BlockChecked;
    /** Notifies listeners that a key for mining is required (coinbase) */
    boost::signals2::signal<void(boost::shared_ptr<CReserveScript> &)> ScriptForMining;
    /** Notifies listeners that a block has been successfully mined */
    boost::signals2::signal<void(const uint256 &)> BlockFound;
};

CMainSignals &GetMainSignals();

#endif // NEXA_VALIDATIONINTERFACE_H
