// Copyright (c) 2018-2021 The Bitcoin Unlimited developers
// Copyright (C) 2019-2020 Tom Zander <tomz@freedommail.ch>
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txadmission.h"
#include "DoubleSpendProof.h"
#include "DoubleSpendProofStorage.h"
#include "blockstorage/blockstorage.h"
#include "connmgr.h"
#include "consensus/tx_verify.h"
#include "core_io.h"
#include "dosman.h"
#include "fastfilter.h"
#include "init.h"
#include "main.h"
#include "net.h"
#include "policy/mempool.h"
#include "requestManager.h"
#include "respend/respenddetector.h"
#include "threadgroup.h"
#include "timedata.h"
#include "txmempool.h"
#include "txorphanpool.h"
#include "unlimited.h"
#include "util.h"
#include "utiltime.h"
#include "validation/validation.h"
#include "validationinterface.h"
#include "wallet/wallet.h"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/thread/thread.hpp>
#include <map>
#include <mutex>
#include <string>
#include <vector>

extern CTweak<uint32_t> minRelayFee;
extern CTweak<uint32_t> limitFreeRelay;

using namespace std;

static void TestConflictEnqueueTx(CTxInputData &txd);

// The average commit batch size is used to limit the quantity of transactions that are moved from the defer queue
// onto the inqueue.  Without this, if received transactions far outstrip processing capacity, transactions can be
// shuffled between the in queue and the defer queue with little progress being made.
const uint64_t minCommitBatchSize = 10000;

// avgCommitBatchSize is write protected by csCommitQ and is wrapped in std::atomic for reads.
std::atomic<uint64_t> avgCommitBatchSize(0);

Snapshot txHandlerSnap;

void ThreadCommitToMempool();
void ProcessOrphans(std::vector<CTransactionRef> &vWorkQueue);

CTransactionRef CommitQGet(uint256 hash)
{
    std::unique_lock<std::mutex> lock(csCommitQ);
    std::map<uint256, CTxCommitData>::iterator it = txCommitQ->find(hash);
    if (it == txCommitQ->end())
        return nullptr;
    return it->second.entry.GetSharedTx();
}

static inline uint256 IncomingConflictHash(const COutPoint &prevout) { return prevout.hash; }

void InitTxAdmission()
{
    if (txCommitQ == nullptr)
        txCommitQ = new std::map<uint256, CTxCommitData>();
    txHandlerSnap.Load(); // Get an initial view for the transaction processors
}

void StartTxAdmissionThreads()
{
    // Start incoming transaction processing threads
    for (unsigned int i = 0; i < numTxAdmissionThreads.Value(); i++)
    {
        threadGroup.create_thread(&ThreadTxAdmission);
    }

    // Start tx commitment thread
    threadGroup.create_thread(&ThreadCommitToMempool);

    // Start mempool transaction rate statistics processing thread
    threadGroup.create_thread(&ThreadUpdateTransactionRateStatistics);
}

void StopTxAdmission()
{
    cvTxInQ.notify_all();
    cvCommitQ.notify_all();
    // cvCommitted.notify_all();
}

void FlushTxAdmission()
{
    bool empty = false;

    while (!empty)
    {
        do // give the tx processing threads a chance to run
        {
            {
                LOCK(csTxInQ);
                empty = txInQ.empty() & txDeferQ.empty();
            }
            if (!empty)
                MilliSleep(100);
        } while (!empty);

        {
            std::unique_lock<std::mutex> lock(csCommitQ);
            do // wait for the commit thread to commit everything
            {
                cvCommitQ.wait_for(lock, std::chrono::milliseconds(100));
            } while (!txCommitQ->empty());
            // cvCommitted.notify_all();
        }

        { // block everything and check
            CORRAL(txProcessingCorral, CORRAL_TX_PAUSE);
            {
                LOCK(csTxInQ);
                empty = txInQ.empty() & txDeferQ.empty();
            }
            {
                std::unique_lock<std::mutex> lock(csCommitQ);
                empty &= txCommitQ->empty();
            }
        }
    }
}

// Put the tx on the tx admission queue for processing
void EnqueueTxForAdmission(CTxInputData &txd)
{
    LOCK(csTxInQ);
    // If I have lots of deferred tx, its probably because there's too much volume, so defer new ones right away
    if (txDeferQ.size() > 1000)
    {
        txDeferQ.push(txd);
        return;
    }

    // Otherwise go ahead and put them on the queue
    TestConflictEnqueueTx(txd);
}

static void TestConflictEnqueueTx(CTxInputData &txd)
{
    bool conflict = false;
    for (auto &inp : txd.tx->vin)
    {
        uint256 hash = IncomingConflictHash(inp.prevout);
        if (!incomingConflicts.checkAndSet(hash))
        {
            conflict = true;
            break;
        }
    }

    // If there is no conflict then the transaction is ready for validation and can be placed in the processing
    // queue. However, if there is a conflict then this could be a double spend, so defer the transaction until the
    // transaction it conflicts with has been fully processed.
    if (!conflict)
    {
        // LOG(MEMPOOL, "Enqueue for processing %x\n", txd.tx->GetId().ToString());
        txInQ.push(txd); // add this transaction onto the processing queue.
        cvTxInQ.notify_one();
    }
    else
    {
        LOG(MEMPOOL, "Fastfilter collision, deferred %x\n", txd.tx->GetId().ToString());
        txDeferQ.push(txd);

        // By notifying the commitQ, the deferred queue can be processed right way which helps
        // to forward double spends as quickly as possible.
        cvCommitQ.notify_one();
    }
}


unsigned int TxAlreadyHave(const CInv &inv)
{
    switch (inv.type)
    {
    case MSG_TX:
    {
        if (txRecentlyInBlock.contains(inv.hash))
            return 1;
        if (recentRejects.contains(inv.hash))
            return 2;
        {
            std::unique_lock<std::mutex> lock(csCommitQ);
            const auto &elem = txCommitQ->find(inv.hash);
            if (elem != txCommitQ->end())
            {
                return 5;
            }
        }
        if (mempool.exists(inv.hash))
            return 3;
        if (orphanpool.AlreadyHaveOrphan(inv.hash))
            return 4;
        return 0;
    }
    case MSG_DOUBLESPENDPROOF:
        return mempool.doubleSpendProofStorage()->exists(inv.hash) ||
               mempool.doubleSpendProofStorage()->isRecentlyRejectedProof(inv.hash);
    }
    DbgAssert(0, return false); // this fn should only be called if CInv is a tx
}

void ThreadCommitToMempool()
{
    while (shutdown_threads.load() == false)
    {
        {
            std::unique_lock<std::mutex> lock(csCommitQ);
            do
            {
                cvCommitQ.wait_for(lock, std::chrono::milliseconds(2000));
                if (shutdown_threads.load() == true)
                {
                    return;
                }
            } while (txCommitQ->empty() && txDeferQ.empty());
        }

        {
            if (shutdown_threads.load() == true)
            {
                return;
            }

            CORRAL(txProcessingCorral, CORRAL_TX_COMMITMENT);
            {
                CommitTxToMempool();
                LOG(MEMPOOL, "MemoryPool sz %u txn, %u kB\n", mempool.size(), mempool.DynamicMemoryUsage() / 1000);
                LimitMempoolSize(mempool, maxTxPool.Value() * ONE_MEGABYTE, txPoolExpiry.Value() * 60 * 60);

                CValidationState state;
                FlushStateToDisk(state, FLUSH_STATE_PERIODIC);

                // The flush to disk above is only periodic therefore we need to check if we need to trim
                // any excess from the cache.
                if (pcoinsTip->DynamicMemoryUsage() > (size_t)nCoinCacheMaxSize)
                    pcoinsTip->Trim(nCoinCacheMaxSize * .95);
            }

            mempool.check(pcoinsTip);
        }
    }
}

void LimitMempoolSize(CTxMemPool &pool, size_t limit, unsigned long age)
{
    std::vector<COutPoint> vCoinsToUncache;
    int expired = pool.Expire(GetTime() - age, vCoinsToUncache);
    for (const COutPoint &txin : vCoinsToUncache)
        pcoinsTip->Uncache(txin);
    if (expired != 0)
        LOG(MEMPOOL, "Expired %i transactions from the memory pool\n", expired);

    std::vector<COutPoint> vNoSpendsRemaining;
    pool.TrimToSize(limit, &vNoSpendsRemaining, false);
    for (const COutPoint &removed : vNoSpendsRemaining)
        pcoinsTip->Uncache(removed);
}

void CommitTxToMempool()
{
    // Committing the tx to the mempool takes time.  We can continue to validate non-conflicting tx during this time.
    // To do so, before the transactions are finally commited to the mempool the txCommitQ pointer is copied
    // to txCommitQFinal so that the lock on txCommitQ can be released and processing can continue.
    // However, the incomingConflicts detector is not reset until all the transactions are committed to the mempool.
    std::map<uint256, CTxCommitData> *txCommitQFinal = nullptr;

    std::vector<CTransactionRef> vWhatChanged;
    {
        // We must hold the mempool lock for the duration because we want to be sure that we don't end up
        // doing this loop in the middle of a reorg where we might be clearing the mempool.
        WRITELOCK(mempool.cs_txmempool);

        {
            std::unique_lock<std::mutex> lock(csCommitQ);
            avgCommitBatchSize = (avgCommitBatchSize * 24 + txCommitQ->size()) / 25;
            txCommitQFinal = txCommitQ;
            txCommitQ = new std::map<uint256, CTxCommitData>();
        }

        // These transactions have already been validated so store them directly into the mempool.
        for (auto &it : *txCommitQFinal)
        {
            CTxCommitData &data = it.second;
            mempool._addUnchecked(data.entry, !IsInitialBlockDownload());
            vWhatChanged.push_back(data.entry.GetSharedTx());

            // Indicate that this tx was fully processed/accepted and can now be removed from the req mgr.
            requester.Received(CInv(MSG_TX, data.hash), nullptr);
        }
    }
#ifdef ENABLE_WALLET
    for (auto &it : *txCommitQFinal)
    {
        CTxCommitData &data = it.second;
        SyncWithWallets(data.entry.GetSharedTx(), nullptr, -1);
    }
#endif
    txCommitQFinal->clear();
    delete txCommitQFinal;

    std::map<uint256, CTxInputData> mapWasDeferred;
    {
        LOCK(csTxInQ);
        // Clear the filter of incoming conflicts, and put all queued tx on the deferred queue since they've been
        // deferred
        LOG(MEMPOOL, "txadmission incoming filter reset.  Current txInQ size: %d\n", txInQ.size());
        incomingConflicts.reset();
        while (!txInQ.empty())
        {
            txDeferQ.push(txInQ.front());
            txInQ.pop();
        }

        // Move the previously deferred txns into active processing.

        // We MUST push the first item in the defer queue to the input queue without checking it against incoming
        // conflicts.  This is fine because the first insert into an empty incomingConflicts must succeed.
        // A transaction's inputs could cause a false positive match against each other.  By pushing the first
        // deferred tx without checking, we can still use the efficient fastfilter checkAndSet function for most queue
        // filter checking but mop up the extremely rare tx whose inputs have false positive matches here.
        if (!txDeferQ.empty())
        {
            const CTxInputData &first = txDeferQ.front();

            for (const auto &inp : first.tx->vin)
            {
                uint256 hash = IncomingConflictHash(inp.prevout);
                incomingConflicts.insert(hash);
            }
            txInQ.push(first);
            cvTxInQ.notify_one();
            txDeferQ.pop();
        }

        // Use a map to store the txns so that we end up removing duplicates which could have arrived
        // from re-requests.
        LOG(MEMPOOL, "popping txdeferQ, size %d\n", txDeferQ.size());
        // this could be a lot more efficient
        uint64_t count = 0;
        uint64_t maxmove = max(avgCommitBatchSize * 2, minCommitBatchSize);
        while ((!txDeferQ.empty()) && (count < maxmove))
        {
            count++;
            const uint256 &hash = txDeferQ.front().tx->GetId();
            mapWasDeferred.emplace(hash, txDeferQ.front());
            txDeferQ.pop();
        }
    }

    if (!mapWasDeferred.empty())
        LOG(MEMPOOL, "Enqueueing %d deferred tx\n", mapWasDeferred.size());

    {
        LOCK(csTxInQ);
        for (auto &it : mapWasDeferred)
        {
            // LOG(MEMPOOL, "attempt enqueue deferred %s\n", it.first.ToString());
            TestConflictEnqueueTx(it.second);
        }
    }
    ProcessOrphans(vWhatChanged);
}

void ThreadTxAdmission()
{
    // Process at most this many transactions before letting the commit thread take over
    const int maxTxPerRound = 200;

    while (shutdown_threads.load() == false)
    {
        // Start or Stop threads as determined by the numTxAdmissionThreads tweak
        {
            static CCriticalSection cs_threads;
            static uint32_t numThreads GUARDED_BY(cs_threads) = numTxAdmissionThreads.Value();
            LOCK(cs_threads);
            if (numTxAdmissionThreads.Value() >= 1 && numThreads > numTxAdmissionThreads.Value())
            {
                // Kill this thread
                numThreads--;
                LOGA("Stopping a tx admission thread: Current admission threads are %d\n", numThreads);

                return;
            }
            else if (numThreads < numTxAdmissionThreads.Value())
            {
                // Launch another thread
                numThreads++;
                threadGroup.create_thread(&ThreadTxAdmission);
                LOGA("Starting a new tx admission thread: Current admission threads are %d\n", numThreads);
            }
        }

        // Loop processing starts here
        bool acceptedSomething = false;
        if (shutdown_threads.load() == true)
        {
            return;
        }

        bool fMissingInputs = false;
        CValidationState state;
        // Snapshot ss;
        CTxInputData txd;

        {
            CCriticalBlock lock(csTxInQ, "csTxInQ", __FILE__, __LINE__, LockType::RECURSIVE_MUTEX);
            while (txInQ.empty() && shutdown_threads.load() == false)
            {
                if (shutdown_threads.load() == true)
                {
                    return;
                }
                cvTxInQ.wait(csTxInQ);
            }
            if (shutdown_threads.load() == true)
            {
                return;
            }
        }

        {
            CORRAL(txProcessingCorral, CORRAL_TX_PROCESSING);

            for (unsigned int txPerRoundCount = 0; txPerRoundCount < maxTxPerRound; txPerRoundCount++)
            {
                // tx must be popped within the TX_PROCESSING corral or the state break between processing
                // and commitment will not be clean
                {
                    CCriticalBlock lock(csTxInQ, "csTxInQ", __FILE__, __LINE__, LockType::RECURSIVE_MUTEX);
                    if (txInQ.empty())
                    {
                        // speed up tx chunk processing when there is nothing else to do
                        if (acceptedSomething)
                            cvCommitQ.notify_all();
                        break;
                    }

                    // Make a copy so we can pop and release
                    txd = txInQ.front();
                    txInQ.pop();
                }

                CTransactionRef tx = txd.tx;
                CInv inv(MSG_TX, tx->GetId());

                if (!TxAlreadyHave(inv))
                {
                    std::vector<COutPoint> vCoinsToUncache;
                    bool isRespend = false;
                    if (ParallelAcceptToMemoryPool(txHandlerSnap, mempool, state, tx, true, &fMissingInputs, false,
                            TransactionClass::DEFAULT, vCoinsToUncache, &isRespend, nullptr))
                    {
                        acceptedSomething = true;
                        RelayTransaction(tx);

                        // LOG(MEMPOOL, "Accepted tx: peer=%s: accepted %s onto Q\n", txd.nodeName,
                        //    tx->GetHash().ToString());
                    }
                    else
                    {
                        LOG(MEMPOOL, "Rejected tx: %s(%d) %s: %s. peer %s  hash %s \n", state.GetRejectReason(),
                            state.GetRejectCode(), fMissingInputs ? "orphan" : "", state.GetDebugMessage(),
                            txd.nodeName, tx->GetId().ToString());

                        if (fMissingInputs)
                        {
                            WRITELOCK(orphanpool.cs_orphanpool);
                            orphanpool.AddOrphanTx(tx, txd.nodeId);

                            // DoS prevention: do not allow mapOrphanTransactions to grow unbounded
                            const uint64_t nMaxOrphanPoolSize = maxTxPool.Value() * ONE_MEGABYTE / 10;
                            unsigned int nEvicted =
                                orphanpool.LimitOrphanTxSize(maxOrphanPool.Value(), nMaxOrphanPoolSize);
                            if (nEvicted > 0)
                                LOG(MEMPOOL, "mapOrphan overflow, removed %u tx\n", nEvicted);
                        }
                        else
                        {
                            recentRejects.insert(tx->GetId());

                            if (txd.whitelisted && GetBoolArg("-whitelistforcerelay", DEFAULT_WHITELISTFORCERELAY))
                            {
                                // Always relay transactions received from whitelisted peers, even
                                // if they were already in the mempool or rejected from it due
                                // to policy, allowing the node to function as a gateway for
                                // nodes hidden behind it.
                                //
                                // Never relay transactions that we would assign a non-zero DoS
                                // score for, as we expect peers to do the same with us in that
                                // case.
                                int nDoS = 0;
                                if (!state.IsInvalid(nDoS) || nDoS == 0)
                                {
                                    LOGA("Force relaying tx %s from whitelisted peer=%s\n", tx->GetId().ToString(),
                                        txd.nodeName);
                                    RelayTransaction(tx);
                                }
                                else
                                {
                                    LOGA("Not relaying invalid transaction %s from whitelisted peer=%s (%s)\n",
                                        tx->GetId().ToString(), txd.nodeName, FormatStateMessage(state));
                                }
                            }
                            // If the problem wasn't that the tx is an orphan, then uncache the inputs since we likely
                            // won't
                            // need them again.
                            for (const COutPoint &remove : vCoinsToUncache)
                                pcoinsTip->Uncache(remove);
                        }

                        // Mark tx as received if invalid or an orphan. If it's a valid Tx we mark it received
                        // only when it's finally accepted into the mempool.
                        requester.Received(inv, nullptr);
                    }

                    int nDoS = 0;
                    if (state.IsInvalid(nDoS))
                    {
                        LOG(MEMPOOL, "%s from peer=%s was not accepted: %s\ntx: %s", tx->GetId().ToString(),
                            txd.nodeName, FormatStateMessage(state), EncodeHexTx(*tx));
                        if (state.GetRejectCode() <
                            REJECT_INTERNAL) // Never send AcceptToMemoryPool's internal codes over P2P
                        {
                            CNodeRef from = connmgr->FindNodeFromId(txd.nodeId);
                            if (from)
                            {
                                std::string strCommand = NetMsgType::TX;
                                from->PushMessage(NetMsgType::REJECT, strCommand, (unsigned char)state.GetRejectCode(),
                                    state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), inv.hash);
                                if (nDoS > 0)
                                {
                                    dosMan.Misbehaving(from.get(), nDoS);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}


bool AcceptToMemoryPool(CTxMemPool &pool,
    CValidationState &state,
    const CTransactionRef &tx,
    bool fLimitFree,
    bool *pfMissingInputs,
    bool fRejectAbsurdFee,
    TransactionClass allowedTx)
{
    std::vector<COutPoint> vCoinsToUncache;
    bool res = false;

    // pause parallel tx entry and commit all txns to the pool so that there are no
    // other threads running txadmission and to ensure that the mempool state is current.
    CORRAL(txProcessingCorral, CORRAL_TX_PAUSE);
    CommitTxToMempool();

    {
        // This lock is here to serialize AcceptToMemoryPool(). This must be done because
        // we do not enqueue the transaction prior to calling this function, as we do with
        // the normal multi-threaded tx admission.
        static CCriticalSection cs_accept;
        LOCK(cs_accept);
        txHandlerSnap.Load();

        bool isRespend = false;
        bool missingInputs = false;
        res = ParallelAcceptToMemoryPool(txHandlerSnap, pool, state, tx, fLimitFree, &missingInputs, fRejectAbsurdFee,
            allowedTx, vCoinsToUncache, &isRespend, nullptr);

        // Uncache any coins for txns that failed to enter the mempool but were NOT orphan txns
        if (isRespend || (!res && !missingInputs))
        {
            for (const COutPoint &remove : vCoinsToUncache)
                pcoinsTip->Uncache(remove);
        }

        // Do this commit inside the cs_accept lock to ensure that this function retains its original sequential
        // behavior
        if (res)
            CommitTxToMempool();

        if (pfMissingInputs)
            *pfMissingInputs = missingInputs;
    }
    if (res)
    {
        RelayTransaction(tx);
        LimitMempoolSize(mempool, maxTxPool.Value() * ONE_MEGABYTE, txPoolExpiry.Value() * 60 * 60);
    }
    return res;
}

bool ParallelAcceptToMemoryPool(Snapshot &ss,
    CTxMemPool &pool,
    CValidationState &state,
    const CTransactionRef &tx,
    bool fLimitFree,
    bool *pfMissingInputs,
    bool fRejectAbsurdFee,
    TransactionClass allowedTx,
    std::vector<COutPoint> &vCoinsToUncache,
    bool *isRespend,
    CValidationDebugger *debugger)
{
    const CChainParams &chainparams = Params();

    if (isRespend)
        *isRespend = false;
    unsigned int nSigOps = 0;
    ValidationResourceTracker resourceTracker;
    unsigned int nSize = 0;
    uint64_t start = GetStopwatch();
    if (pfMissingInputs)
        *pfMissingInputs = false;

    if (debugger)
    {
        debugger->txid = tx->GetId().ToString();
    }

    if (!CheckTransaction(tx, state) || !ContextualCheckTransaction(tx, state, chainActive.Tip(), chainparams))
    {
        if (state.GetDebugMessage() == "")
            state.SetDebugMessage("CheckTransaction failed");
        if (debugger)
        {
            debugger->AddInvalidReason(state.GetRejectReason());
            state = CValidationState();
            debugger->mineable = false;
            // assume a tx that does not pass validation is non standard and not future mineable
            debugger->futureMineable = false;
            debugger->standard = false;
        }
        else
        {
            return false;
        }
    }

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx->IsCoinBase())
    {
        if (debugger)
        {
            debugger->AddInvalidReason("Coinbase is only valid in a block, not as a loose transaction");
            debugger->mineable = false;
            debugger->futureMineable = false;
        }
        else
        {
            return state.DoS(100, false, REJECT_INVALID, "coinbase");
        }
    }

    // Reject nonstandard transactions if so configured.
    // (-testnet/-regtest allow nonstandard, and explicit submission via RPC)
    std::string reason;
    bool fRequireStandard = chainparams.RequireStandard();

    if (allowedTx == TransactionClass::STANDARD)
    {
        fRequireStandard = true;
    }
    else if (allowedTx == TransactionClass::NONSTANDARD)
    {
        fRequireStandard = false;
    }
    if (fRequireStandard && !IsStandardTx(tx, reason))
    {
        if (debugger)
        {
            debugger->AddInvalidReason(reason);
            // if we require standard, a non standard tx is not mineable or future mineable
            debugger->mineable = false;
            debugger->futureMineable = false;
            debugger->standard = false;
        }
        else
        {
            state.SetDebugMessage("IsStandardTx failed");
            return state.DoS(0, false, REJECT_NONSTANDARD, reason);
        }
    }

    uint32_t featureFlags = 0;
    // featureFlags is used in some instances to make a hardfork easier to implement
    // Example: featureFlags |= SCRIPT_VERIFY_INPUT_SIGCHECKS;
    uint32_t flags = STANDARD_SCRIPT_VERIFY_FLAGS | featureFlags;

    // Only accept nLockTime-using transactions that can be mined in the next
    // block; we don't want our mempool filled up with transactions that can't
    // be mined yet.
    if (!CheckFinalTx(tx, STANDARD_LOCKTIME_VERIFY_FLAGS, &ss))
    {
        if (debugger)
        {
            debugger->AddInvalidReason("non-final");
            // non final is not mineable now but may be in the future
            debugger->mineable = false;
        }
        else
        {
            return state.DoS(0, false, REJECT_NONSTANDARD, "non-final");
        }
    }

    // Make sure tx size is acceptable
    {
        if (tx->GetTxSize() < MIN_TX_SIZE)
        {
            if (debugger)
            {
                debugger->AddInvalidReason("txn-undersize");
                debugger->mineable = false;
                debugger->futureMineable = false;
            }
            else
            {
                return state.DoS(0, false, REJECT_INVALID, "txn-undersize");
            }
        }
    }

    // is it already in the memory pool?
    uint256 id = tx->GetId();
    uint256 idem = tx->GetIdem();
    if (pool.idemExists(idem))
    {
        if (debugger)
        {
            debugger->AddInvalidReason("txn-already-in-mempool");
        }
        else
        {
            return state.Invalid(false, REJECT_ALREADY_KNOWN, "txn-already-in-mempool");
        }
    }

    // Check for conflicts with in-memory transactions and triggers actions at
    // end of scope (relay tx, sync wallet, etc)
    respend::RespendDetector respend(pool, tx);
    *isRespend = respend.IsRespend();

    if (respend.IsRespend() && !respend.IsInteresting())
    {
        if (debugger)
        {
            // debugger->AddInvalidReason(
            // "tx-mempool-conflict: " + txin.prevout.hash.ToString() + ":" + std::to_string(txin.prevout.n));
            debugger->AddInvalidReason("txn-txpool-conflict");
            debugger->mineable = false;
            debugger->futureMineable = false;
        }
        else
        {
            // Tx is a respend, and it's not an interesting one (we don't care to
            // validate it further)
            return state.Invalid(false, REJECT_CONFLICT, "txn-txpool-conflict");
        }
    }
    {
        CCoinsView dummy;
        CCoinsViewCache view(&dummy);

        CAmount nValueIn = 0;
        LockPoints lp;
        {
            READLOCK(ss.cs_snapshot);
            READLOCK(pool.cs_txmempool);
            CCoinsViewMemPool &viewMemPool(*ss.cvMempool);
            view.SetBackend(viewMemPool);
            // do all inputs exist?
            if (pfMissingInputs)
            {
                *pfMissingInputs = false;
                for (const CTxIn &txin : tx->vin)
                {
                    // At this point we begin to collect coins that are potential candidates for uncaching because as
                    // soon as we make the call below to view.HaveCoin() any missing coins will be pulled into cache.
                    // Therefore, any coin in this transaction that is not already in cache will be tracked here such
                    // that if this transaction fails to enter the memory pool, we will then uncache those coins that
                    // were not already present, unless the transaction is an orphan.
                    //
                    // We still want to keep orphantx coins in the event the orphantx is finally accepted into the
                    // mempool or shows up in a block that is mined.  Therefore if pfMissingInputs returns true then
                    // any coins in vCoinsToUncache will NOT be uncached.
                    bool fSpent = false;
                    bool fMissingOrSpent = false;
                    if (!ss.coins->HaveCoinInCache(txin.prevout, fSpent))
                    {
                        vCoinsToUncache.push_back(txin.prevout);
                        if (!view.GetCoinFromDB(txin.prevout))
                        {
                            fMissingOrSpent = true;
                        }
                    }
                    if (fSpent || fMissingOrSpent)
                    {
                        if (debugger)
                        {
                            debugger->AddInvalidReason("input-does-not-exist: " + txin.prevout.hash.ToString());
                            // missing inputs are not mineable now. it may be mineable
                            // in the future but it is not certain, assume it will not be
                            debugger->mineable = false;
                            debugger->futureMineable = false;
                        }
                        // fMissingInputs and not state.IsInvalid() is used to detect this condition, don't set
                        // state.Invalid()
                        *pfMissingInputs = true;
                        if (debugger == nullptr)
                        {
                            break; // There is no point checking any more once one fails, for orphans we will recheck
                        }
                    }
                }
                if (*pfMissingInputs == true)
                {
                    if (debugger)
                    {
                        debugger->AddInvalidReason("inputs-are-missing");
                        // missing inputs are not mineable now. it may be mineable
                        // in the future but it is not certain, assume it will not be
                        debugger->mineable = false;
                        debugger->futureMineable = false;
                        return false;
                    }
                    else
                    {
                        state.SetDebugMessage("inputs-are-missing");
                        return false; // state.Invalid(false, REJECT_MISSING_INPUTS, "bad-txns-missing-inputs", "Inputs
                        // unavailable in ParallelAcceptToMemoryPool", false);
                    }
                }
            }

            // Bring the best block into scope
            view.GetBestBlock();

            nValueIn = view.GetValueIn(*tx);

            // we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
            view.SetBackend(dummy);

            // Only accept BIP68 sequence locked transactions that can be mined in the next
            // block; we don't want our mempool filled up with transactions that can't
            // be mined yet.
            // Must keep pool.cs for this unless we change CheckSequenceLocks to take a
            // CoinsViewCache instead of create its own
            if (!CheckSequenceLocks(tx, STANDARD_LOCKTIME_VERIFY_FLAGS, &lp, false, &ss))
            {
                if (debugger)
                {
                    debugger->AddInvalidReason("non-BIP68-final");
                    // not mineable now but should be in the future
                    debugger->mineable = false;
                }
                else
                {
                    return state.DoS(0, false, REJECT_NONSTANDARD, "non-BIP68-final");
                }
            }
        }

        // Check for non-standard pay-to-script-hash in inputs
        if (fRequireStandard && !AreInputsStandard(tx, view))
        {
            if (debugger)
            {
                debugger->AddInvalidReason("bad-txns-nonstandard-inputs");
                debugger->standard = false;
                // if we require standard and this tx is not standard, we can not
                // mine now or in the future either
                debugger->mineable = false;
                debugger->futureMineable = false;
            }
            else
            {
                return state.Invalid(false, REJECT_NONSTANDARD, "bad-txns-nonstandard-inputs");
            }
        }

        CAmount nValueOut = tx->GetValueOut();
        CAmount nFees = nValueIn - nValueOut;
        // nModifiedFees includes any fee deltas from PrioritiseTransaction
        CAmount nModifiedFees = nFees;
        double nPriorityDummy = 0;
        // Search either id or idem for a user-applied priority modifier
        pool.ApplyDeltas(id, nPriorityDummy, nModifiedFees);
        pool.ApplyDeltas(idem, nPriorityDummy, nModifiedFees);

        CAmount inChainInputValue;
        double dPriority = view.GetPriority(*tx, chainActive.Height(), inChainInputValue);

        // Keep track of transactions that spend a coinbase, which we re-scan
        // during reorgs to ensure COINBASE_MATURITY is still met.
        bool fSpendsCoinbase = false;
        for (const CTxIn &txin : tx->vin)
        {
            CoinAccessor coin(view, txin.prevout);
            if (coin->IsCoinBase())
            {
                fSpendsCoinbase = true;
                break;
            }
        }

        // Check that input script constraints are satisfied
        unsigned char sighashType = 0;
        if (!CheckInputs(
                tx, state, view, true, flags, true, &resourceTracker, chainparams, nullptr, &sighashType, debugger))
        {
            if (debugger && debugger->InputsCheck1IsValid())
            {
                debugger->AddInvalidReason("input-script-failed");
                debugger->mineable = false;
                debugger->futureMineable = false;
            }
            else
            {
                LOG(MEMPOOL, "CheckInputs failed for tx: %s\n", id.ToString());
                if (state.GetDebugMessage() == "")
                    state.SetDebugMessage("CheckInputs failed");
                return false;
            }
        }

        // Check that the transaction doesn't have an excessive number of sigops, making it impossible to mine.
        {
            nSigOps = resourceTracker.GetConsensusSigChecks();
            if (nSigOps > MAX_TX_SIGCHECK_COUNT)
            {
                if (debugger)
                {
                    debugger->AddInvalidReason("bad-txns-too-many-sigchecks");
                    // not mineable now or in the future under the current ruleset
                    debugger->mineable = false;
                    debugger->futureMineable = false;
                }
                else
                {
                    return state.DoS(
                        0, false, REJECT_INVALID, "bad-txns-too-many-sigchecks", false, strprintf("%d", nSigOps));
                }
            }
            // Place sigchecks into the mempool sigops field, since these are not cotemporaneous
            LOG(MEMPOOL, "Mempool is tracking sigchecks.  Tx %s has %d\n", id.ToString(), nSigOps);
        }

        // Create a commit data entry
        CTxMemPoolEntry entry(tx, nFees, GetTime(), dPriority, chainActive.Height(), pool.HasNoInputsOf(tx),
            inChainInputValue, fSpendsCoinbase, nSigOps, lp);
        // Record the actual number of sigops executed for statistical purposes only
        entry.UpdateRuntimeSigOps(resourceTracker.GetSigOps(), resourceTracker.GetSighashBytes());

        nSize = entry.GetTxSize();
        if (GetBoolArg("-relaypriority", DEFAULT_RELAYPRIORITY) && nModifiedFees < ::minRelayTxFee.GetFee(nSize) &&
            !AllowFree(entry.GetPriority(chainActive.Height() + 1)))
        {
            if (debugger)
            {
                debugger->AddInvalidReason("insufficient-priority");
                debugger->AddInvalidReason("insufficient-fee: need " + std::to_string(minRelayTxFee.GetFee(nSize)) +
                                           " was only " + std::to_string(nModifiedFees));
                debugger->AddInvalidReason("minimum-fee: " + std::to_string(minRelayTxFee.GetFee(nSize)));
                // fees are not a reason to mark something as not mineable, keep current mineable and futureMineable
                // values unchanged
                debugger->standard = false;
            }
            else
            {
                // Require that free transactions have sufficient priority to be mined in the next block.
                LOG(MEMPOOL, "Txn fee %lld (%d - %d), priority fee delta was %lld\n", nFees, nValueIn, nValueOut,
                    nModifiedFees - nFees);
                return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "insufficient priority");
            }
        }
        if (debugger)
        {
            debugger->txMetadata.emplace("size", std::to_string(nSize));
            debugger->txMetadata.emplace("txfee", std::to_string(nModifiedFees));
            debugger->txMetadata.emplace("txfeeneeded", std::to_string(minRelayTxFee.GetFee(nSize)));
        }

        /* Continuously rate-limit free (really, very-low-fee) transactions
         * This mitigates 'penny-flooding' -- sending thousands of free transactions just to
         * be annoying or make others' transactions take longer to confirm. */
        static CCriticalSection cs_limiter;
        {
            LOCK(cs_limiter);

            static int64_t nLastTime = GetTime();
            int64_t nNow = GetTime();
            minRelayTxFee = CFeeRate((CAmount)(minRelayFee.Value()));

            // useful but spammy
            // LOG(MEMPOOL,
            //    "MempoolBytes:%ld LimitFreeRelay:%d FeesSatoshiPerKB:%ld TxBytes:%d "
            //    "TxFees:%ld\n",
            //    pool.GetTotalTxSize(), limitFreeRelay.Value(), nModifiedFees * 1000 / nSize, nSize, nModifiedFees);
            if (fLimitFree && nModifiedFees < ::minRelayTxFee.GetFee(nSize))
            {
                static double dFreeCount = 0;

                // Use an exponentially decaying ~10-minute window:
                dFreeCount *= std::pow(1.0 - 1.0 / 600.0, (double)(nNow - nLastTime));
                nLastTime = nNow;

                // limitFreeRelay is in KB per minute but we multiply it
                //  by an extra 10 because we're using a 10 minute decay window.
                LOG(MEMPOOL, "Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount + nSize);
                if ((dFreeCount + nSize) >= (limitFreeRelay.Value() * 10 * 1000))
                {
                    if (debugger)
                    {
                        debugger->AddInvalidReason("rate limited free transaction");
                        // fees are not a reason to mark something as not mineable, keep current mineable and
                        // futureMineable values unchanged
                        debugger->standard = false;
                    }
                    else
                    {
                        LOG(MEMPOOL, "AcceptToMemoryPool : free transaction %s rejected by rate limiter\n",
                            id.ToString());
                        return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "mempool min fee not met");
                    }
                }
                dFreeCount += nSize;
            }
            else if (nModifiedFees < ::minRelayTxFee.GetFee(nSize))
            {
                if (debugger)
                {
                    debugger->AddInvalidReason("mempool min fee not met");
                    // fees are not a reason to mark something as not mineable, keep current mineable and futureMineable
                    // values unchanged
                    debugger->standard = false;
                }
                else
                {
                    LOG(MEMPOOL, "AcceptToMemoryPool : min fee not met for %s\n", id.ToString());
                    return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "mempool min fee not met");
                }
            }
        }
#ifdef ENABLE_WALLET
        // We calculate the recommended fee by looking at what's in the mempool.  This starts at 0 though for an
        // empty mempool.  So set the minimum "absurd" fee to 10000 satoshies per byte.  If for some reason fees rise
        // above that, you can specify up to 100x what other txns are paying in the mempool
        if (fRejectAbsurdFee && nFees > std::max((int64_t)100L * nSize, maxTxFeeTweak.Value()) * 100)
        {
            if (debugger)
            {
                debugger->AddInvalidReason("absurdly-high-fee");
                // fees are not a reason to mark something as not mineable, keep current mineable and futureMineable
                // values unchanged
                debugger->standard = false;
            }
            else
            {
                return state.Invalid(false, REJECT_HIGHFEE, "absurdly-high-fee",
                    strprintf("%d > %d", nFees, std::max((int64_t)100L * nSize, maxTxFeeTweak.Value()) * 100));
            }
        }
#endif

        // Check again against just the consensus-critical mandatory script
        // verification flags, in case of bugs in the standard flags that cause
        // transactions to pass as valid when they're actually invalid. For
        // instance the STRICTENC flag was incorrectly allowing certain
        // CHECKSIG NOT scripts to pass, even though they were invalid.
        //
        // There is a similar check in CreateNewBlock() to prevent creating
        // invalid blocks, however allowing such transactions into the mempool
        // can be exploited as a DoS attack.
        unsigned char sighashType2 = 0;
        if (!CheckInputs(tx, state, view, true, MANDATORY_SCRIPT_VERIFY_FLAGS | featureFlags, true, nullptr,
                chainparams, nullptr, &sighashType2, debugger))
        {
            if (debugger && debugger->InputsCheck1IsValid())
            {
                debugger->AddInvalidReason("CheckInputs failed against mandatory but not standard flags");
                debugger->mineable = false;
                debugger->futureMineable = false;
            }
            else
            {
                if (state.GetDebugMessage() == "")
                    state.SetDebugMessage("CheckInputs failed against mandatory but not standard flags");

                return error(
                    "%s: BUG! PLEASE REPORT THIS! ConnectInputs failed against MANDATORY but not STANDARD flags %s, %s",
                    __func__, id.ToString(), FormatStateMessage(state));
            }
        }

        // Check for repend before committing the tx to the mempool
        respend.SetValid(true);
        if (respend.IsRespend())
        {
            if (debugger)
            {
                debugger->AddInvalidReason("txn-mempool-conflict");
            }
            else
            {
                return state.Invalid(false, REJECT_CONFLICT, "txn-mempool-conflict");
            }
        }
        else if (debugger == nullptr)
        {
            // If it's not a respend it may have a reclaimed orphan associated with it
            entry.dsproof = respend.GetDsproof();

            // Add entry to the commit queue
            CTxCommitData eData;
            eData.entry = std::move(entry);
            eData.hash = id;

            std::unique_lock<std::mutex> lock(csCommitQ);
            (*txCommitQ).emplace(eData.hash, eData);
        }
    }
    uint64_t interval = (GetStopwatch() - start) / 1000;
    // typically too much logging, but useful when optimizing tx validation
    LOG(BENCH,
        "ValidateTransaction, time: %d, tx: %s, len: %d, sigops: %llu (legacy: %u), sighash: %llu, Vin: "
        "%llu, Vout: %llu\n",
        interval, tx->GetId().ToString(), nSize, resourceTracker.GetSigOps(), (unsigned int)nSigOps,
        resourceTracker.GetSighashBytes(), tx->vin.size(), tx->vout.size());
    nTxValidationTime << interval;

    // Update txn per second. We must do it here although technically the txn isn't in the mempool yet but
    // rather in the CommitQ. However, if we don't do it here then we'll end up with very bursty and not very
    // realistic processing throughput data.
    mempool.UpdateTransactionsPerSecond();

    return true;
}


TransactionClass ParseTransactionClass(const std::string &s)
{
    std::string low = boost::algorithm::to_lower_copy(s);
    if (low == "nonstandard")
    {
        return TransactionClass::NONSTANDARD;
    }
    if (low == "standard")
    {
        return TransactionClass::STANDARD;
    }
    if (low == "default")
    {
        return TransactionClass::DEFAULT;
    }

    return TransactionClass::INVALID;
}


void ProcessOrphans(std::vector<CTransactionRef> &vWorkQueue)
{
    // Recursively process any orphan transactions that depended on this one.
    // NOTE: you must not return early since EraseOrphansByTime() must always be checked
    std::map<uint256, CTxInputData> mapEnqueue;
    {
        READLOCK(orphanpool.cs_orphanpool);
        for (unsigned int i = 0; i < vWorkQueue.size(); i++)
        {
            CTransactionRef tx = vWorkQueue[i];
            for (unsigned int j = 0; j < tx->vout.size(); j++)
            {
                std::map<uint256, std::set<uint256> >::iterator itByPrev =
                    orphanpool.mapOrphanTransactionsByPrev.find(tx->OutpointAt(j).hash);
                if (itByPrev == orphanpool.mapOrphanTransactionsByPrev.end())
                    continue;
                //            for (std::set<uint256>::iterator mi = itByPrev->second.begin(); mi !=
                //            itByPrev->second.end(); ++mi)
                for (const auto &orphanHash : itByPrev->second)
                {
                    // Make sure we actually have an entry on the orphan cache. While this should never fail because
                    // we always erase orphans and any mapOrphanTransactionsByPrev at the same time, still we need to
                    // be sure.
                    bool fOk = true;
                    std::map<uint256, CTxOrphanPool::COrphanTx>::iterator iter =
                        orphanpool.mapOrphanTransactions.find(orphanHash);
                    DbgAssert(iter != orphanpool.mapOrphanTransactions.end(), fOk = false);
                    if (!fOk)
                        continue;

                    {
                        CTxInputData txd;
                        txd.tx = iter->second.ptx;
                        txd.nodeId = iter->second.fromPeer;
                        txd.nodeName = "orphan";
                        LOG(MEMPOOL, "Resubmitting orphan tx: %s\n", orphanHash.ToString());
                        mapEnqueue.emplace(std::move(orphanHash), std::move(txd));
                    }
                }
            }
        }
    }

    // First delete the orphans before enqueuing them otherwise we may end up putting them
    // in the queue twice.
    orphanpool.EraseOrphansByTime();
    if (!mapEnqueue.empty())
    {
        {
            WRITELOCK(orphanpool.cs_orphanpool);
            for (auto it = mapEnqueue.begin(); it != mapEnqueue.end(); it++)
            {
                // If the orphan was not erased then it must already have been erased/enqueued by another thread
                // so do not enqueue this orphan again.
                if (!orphanpool.EraseOrphanTx(it->first))
                    it = mapEnqueue.erase(it);
            }
        }
        for (auto &it : mapEnqueue)
            EnqueueTxForAdmission(it.second);
    }
}


void Snapshot::Load(void)
{
    WRITELOCK(cs_snapshot);
    tipHeight = chainActive.Height();
    tip = chainActive.Tip();
    if (tip)
    {
        tipMedianTimePast = tip->GetMedianTimePast();
    }
    else
    {
        tipMedianTimePast = 0; // MTP does not matter, we are in IBD
    }
    adjustedTime = GetAdjustedTime();
    coins = pcoinsTip; // TODO pcoinsTip can change
    if (cvMempool)
        delete cvMempool;

    READLOCK(mempool.cs_txmempool);
    // ss.coins contains the UTXO set for the tip in ss
    cvMempool = new CCoinsViewMemPool(coins, mempool);
}

bool CheckSequenceLocks(const CTransactionRef tx,
    int flags,
    LockPoints *lp,
    bool useExistingLockPoints,
    const Snapshot *ss)
{
    if (ss == nullptr)
        AssertLockHeld(cs_main);
    AssertLockHeld(mempool.cs_txmempool);

    CBlockIndex *tip = (ss != nullptr) ? ss->tip : chainActive.Tip();
    CBlockIndex index;
    index.pprev = tip;
    // CheckSequenceLocks() uses chainActive.Height()+1 to evaluate
    // height based locks because when SequenceLocks() is called within
    // ConnectBlock(), the height of the block *being*
    // evaluated is what is used.
    // Thus if we want to know if a transaction can be part of the
    // *next* block, we need to use one more than chainActive.Height()
    index.header.height = tip->height() + 1;

    std::pair<int, int64_t> lockPair;
    if (useExistingLockPoints)
    {
        assert(lp);
        lockPair.first = lp->height;
        lockPair.second = lp->time;
    }
    else
    {
        // pcoinsTip contains the UTXO set for chainActive.Tip()
        CCoinsViewMemPool tmpView(pcoinsTip, mempool);
        CCoinsViewMemPool &viewMemPool = (ss != nullptr) ? *ss->cvMempool : tmpView;
        std::vector<int> prevheights;
        prevheights.resize(tx->vin.size());
        for (size_t txinIndex = 0; txinIndex < tx->vin.size(); txinIndex++)
        {
            const CTxIn &txin = tx->vin[txinIndex];
            Coin coin;
            if (!viewMemPool.GetCoin(txin.prevout, coin))
            {
                return error("%s: Missing input", __func__);
            }
            if (coin.nHeight == MEMPOOL_HEIGHT)
            {
                // Assume all mempool transaction confirm in the next block
                prevheights[txinIndex] = tip->height() + 1;
            }
            else
            {
                prevheights[txinIndex] = coin.height();
            }
        }
        lockPair = CalculateSequenceLocks(tx, flags, &prevheights, index);
        if (lp)
        {
            lp->height = lockPair.first;
            lp->time = lockPair.second;
            // Also store the hash of the block with the highest height of
            // all the blocks which have sequence locked prevouts.
            // This hash needs to still be on the chain
            // for these LockPoint calculations to be valid
            // Note: It is impossible to correctly calculate a maxInputBlock
            // if any of the sequence locked inputs depend on unconfirmed txs,
            // except in the special case where the relative lock time/height
            // is 0, which is equivalent to no sequence lock. Since we assume
            // input height of tip+1 for mempool txs and test the resulting
            // lockPair from CalculateSequenceLocks against tip+1.  We know
            // EvaluateSequenceLocks will fail if there was a non-zero sequence
            // lock on a mempool input, so we can use the return value of
            // CheckSequenceLocks to indicate the LockPoints validity
            int maxInputHeight = 0;
            for (int height : prevheights)
            {
                // Can ignore mempool inputs since we'll fail if they had non-zero locks
                if (height != tip->height() + 1)
                {
                    maxInputHeight = std::max(maxInputHeight, height);
                }
            }
            lp->maxInputBlock = tip->GetAncestor(maxInputHeight);
        }
    }
    return EvaluateSequenceLocks(index, lockPair);
}

bool CheckFinalTx(const CTransactionRef tx, int flags, const Snapshot *ss)
{
    // By convention a negative value for flags indicates that the
    // current network-enforced consensus rules should be used. In
    // a future soft-fork scenario that would mean checking which
    // rules would be enforced for the next block and setting the
    // appropriate flags. At the present time no soft-forks are
    // scheduled, so no flags are set.
    flags = std::max(flags, 0);

    // CheckFinalTx() uses chainActive.Height()+1 to evaluate
    // nLockTime because when IsFinalTx() is called within
    // CBlock::AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a
    // transaction can be part of the *next* block, we need to call
    // IsFinalTx() with one more than chainActive.Height().
    const int64_t nBlockHeight = max((int64_t)((ss != nullptr) ? ss->tipHeight + 1 : 0), chainActive.Height() + 1);

    // BIP113 will require that time-locked transactions have nLockTime set to
    // less than the median time of the previous block they're contained in.
    // When the next block is created its previous block will be the current
    // chain tip, so we use that to calculate the median time passed to
    // IsFinalTx() if LOCKTIME_MEDIAN_TIME_PAST is set.
    const int64_t nMedianTimePast = (ss != nullptr) ? ss->tipMedianTimePast : chainActive.Tip()->GetMedianTimePast();
    const int64_t nBlockTime = (flags & LOCKTIME_MEDIAN_TIME_PAST) ? nMedianTimePast : GetAdjustedTime();

    return IsFinalTx(tx, nBlockHeight, nBlockTime);
}

bool CheckFinalTx(const CTransaction *tx, int flags, const Snapshot *ss)
{
    // By convention a negative value for flags indicates that the
    // current network-enforced consensus rules should be used. In
    // a future soft-fork scenario that would mean checking which
    // rules would be enforced for the next block and setting the
    // appropriate flags. At the present time no soft-forks are
    // scheduled, so no flags are set.
    flags = std::max(flags, 0);

    // CheckFinalTx() uses chainActive.Height()+1 to evaluate
    // nLockTime because when IsFinalTx() is called within
    // CBlock::AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a
    // transaction can be part of the *next* block, we need to call
    // IsFinalTx() with one more than chainActive.Height().
    const int64_t nBlockHeight = max((int64_t)((ss != nullptr) ? ss->tipHeight + 1 : 0), chainActive.Height() + 1);

    // BIP113 will require that time-locked transactions have nLockTime set to
    // less than the median time of the previous block they're contained in.
    // When the next block is created its previous block will be the current
    // chain tip, so we use that to calculate the median time passed to
    // IsFinalTx() if LOCKTIME_MEDIAN_TIME_PAST is set.
    const int64_t nMedianTimePast = (ss != nullptr) ? ss->tipMedianTimePast : chainActive.Tip()->GetMedianTimePast();
    const int64_t nBlockTime = (flags & LOCKTIME_MEDIAN_TIME_PAST) ? nMedianTimePast : GetAdjustedTime();

    return IsFinalTx(tx, nBlockHeight, nBlockTime);
}
