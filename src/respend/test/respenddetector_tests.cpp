// Copyright (c) 2018 The Bitcoin developers
// Copyright (c) 2018-2020 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include "DoubleSpendProofStorage.h"
#include "key.h"
#include "keystore.h"
#include "policy/policy.h"
#include "primitives/transaction.h"
#include "respend/respendaction.h"
#include "respend/respenddetector.h"
#include "script/script.h"
#include "script/script_error.h"
#include "script/sighashtype.h"
#include "script/sign.h"
#include "test/test_nexa.h"
#include "test/testutil.h"
#include "txmempool.h"
#include <boost/test/unit_test.hpp>

using namespace respend;

namespace
{
class DummyRespendAction : public RespendAction
{
public:
    DummyRespendAction()
        : addOutpointCalls(0), respentBefore(false), isEquivalent(false), triggered(false), returnInteresting(false),
          valid(false)
    {
    }

    bool AddOutpointConflict(const COutPoint &out,
        const uint256 hash,
        const CTransactionRef respendTx,
        bool fRespentBefore,
        bool fIsEquivalent) override
    {
        addOutpointCalls++;
        this->respentBefore = fRespentBefore;
        this->isEquivalent = fIsEquivalent;
        return false;
    }

    bool IsInteresting() const override { return returnInteresting; }
    void SetValid(bool v) override { valid = v; }
    void Trigger(CTxMemPool &pool) override { triggered = true; }
    int addOutpointCalls;
    bool respentBefore;
    bool isEquivalent;
    bool triggered;
    bool returnInteresting;
    bool valid;
};

class RespendFixture : public BasicTestingSetup
{
public:
    RespendFixture() : mempool(), dummyaction(new DummyRespendAction) {}
    CTxMemPool mempool;
    std::shared_ptr<DummyRespendAction> dummyaction;
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(respenddetector_tests, RespendFixture);

// Helper: create two dummy transactions, each with
// two outputs.  The first has 11 and 50 CENT outputs
// paid to a TX_PUBKEY, the second 21 and 22 CENT outputs
// paid to a TX_PUBKEYHASH.
//
static std::vector<CMutableTransaction> SetupDummyInputs(CBasicKeyStore &keystoreRet, CCoinsViewCache &coinsRet)
{
    std::vector<CMutableTransaction> dummyTransactions;
    dummyTransactions.resize(2);

    // Add some keys to the keystore:
    CKey key[4];
    for (int i = 0; i < 4; i++)
    {
        key[i].MakeNewKey(i % 2);
        keystoreRet.AddKey(key[i]);
    }

    // Create some dummy input transactions
    int nHeight = 1000; // any height will do
    dummyTransactions[0].vout.resize(2);
    dummyTransactions[0].vout[0].nValue = 50 * CENT;
    dummyTransactions[0].vout[0].scriptPubKey << ToByteVector(key[0].GetPubKey()) << OP_CHECKSIG;
    dummyTransactions[0].vout[1].nValue = 50 * CENT;
    dummyTransactions[0].vout[1].scriptPubKey << ToByteVector(key[1].GetPubKey()) << OP_CHECKSIG;
    AddCoins(coinsRet, dummyTransactions[0], nHeight);

    return dummyTransactions;
}

static void ClearInventory(CNode *pnode)
{
    LOCK(pnode->cs_inventory);
    pnode->vInventoryToSend.clear();
}
BOOST_AUTO_TEST_CASE(not_a_respend)
{
    CMutableTransaction tx1 = CreateRandomTx();
    CMutableTransaction tx2 = CreateRandomTx();

    // Nothing in mempool, can't be a respend.
    {
        RespendDetector detector(mempool, MakeTransactionRef(tx1), {dummyaction});
        BOOST_CHECK(!detector.IsRespend());
        BOOST_CHECK_EQUAL(0, dummyaction->addOutpointCalls);
    }

    TestMemPoolEntryHelper entry;
    mempool.addUnchecked(entry.FromTx(tx1));

    // tx2 is not a respend of tx1
    RespendDetector detector(mempool, MakeTransactionRef(tx2), {dummyaction});
    BOOST_CHECK(!detector.IsRespend());
    BOOST_CHECK_EQUAL(0, dummyaction->addOutpointCalls);
}

BOOST_AUTO_TEST_CASE(only_script_differs)
{
    CMutableTransaction tx1 = CreateRandomTx();
    CMutableTransaction tx2 = tx1;
    tx2.vin[0].scriptSig << OP_DROP << OP_1;

    TestMemPoolEntryHelper entry;
    mempool.addUnchecked(entry.FromTx(tx1));
    RespendDetector detector(mempool, MakeTransactionRef(tx2), {dummyaction});
    BOOST_CHECK(detector.IsRespend());
    // when only the script differs, the isEquivalent flag should be set
    BOOST_CHECK(dummyaction->isEquivalent);
    BOOST_CHECK(!dummyaction->respentBefore);
}

BOOST_AUTO_TEST_CASE(seen_before)
{
    CMutableTransaction tx1 = CreateRandomTx();
    CMutableTransaction tx2 = tx1;
    tx2.vout[0].scriptPubKey = CreateRandomTx().vout[0].scriptPubKey;

    TestMemPoolEntryHelper entry;
    mempool.addUnchecked(entry.FromTx(tx1));

    {
        RespendDetector detector(mempool, MakeTransactionRef(tx2), {dummyaction});
        BOOST_CHECK(detector.IsRespend());
        BOOST_CHECK(!dummyaction->isEquivalent);
        BOOST_CHECK(!dummyaction->respentBefore);

        // only valid txs are added to the seen before filter
        detector.SetValid(true);
    }

    // tx3 differs from tx2, but spends the same input
    CMutableTransaction tx3 = tx1;
    tx3.vout[0].scriptPubKey = CreateRandomTx().vout[0].scriptPubKey;
    {
        RespendDetector detector(mempool, MakeTransactionRef(tx3), {dummyaction});
        BOOST_CHECK(detector.IsRespend());
        BOOST_CHECK(!dummyaction->isEquivalent);
        BOOST_CHECK(dummyaction->respentBefore);
    }
}

BOOST_AUTO_TEST_CASE(triggers_actions)
{
    // Actions should trigger when RespendDetector goes out of scope.
    {
        RespendDetector detector(mempool, MakeTransactionRef(CTransaction{}), {dummyaction});
        BOOST_CHECK(!dummyaction->triggered);
    }
    BOOST_CHECK(dummyaction->triggered);
}

BOOST_AUTO_TEST_CASE(is_interesting)
{
    // Respend is interesting when at least one action finds it interesting.
    auto action1 = new DummyRespendAction;
    auto action2 = new DummyRespendAction;
    RespendDetector detector(
        mempool, MakeTransactionRef(CTransaction{}), {RespendActionPtr(action1), RespendActionPtr(action2)});

    action1->returnInteresting = false;
    action2->returnInteresting = false;
    BOOST_CHECK(!detector.IsInteresting());

    action2->returnInteresting = true;
    BOOST_CHECK(detector.IsInteresting());
}

BOOST_AUTO_TEST_CASE(set_valid)
{
    auto action1 = new DummyRespendAction;
    auto action2 = new DummyRespendAction;
    RespendDetector detector(
        mempool, MakeTransactionRef(CTransaction{}), {RespendActionPtr(action1), RespendActionPtr(action2)});

    detector.SetValid(true);
    BOOST_CHECK(action1->valid);
    BOOST_CHECK(action2->valid);

    detector.SetValid(false);
    BOOST_CHECK(!action1->valid);
    BOOST_CHECK(!action2->valid);
}

BOOST_AUTO_TEST_CASE(dsproof_orphan_handling)
{
    CNode node(INVALID_SOCKET, CAddress());
    node.fRelayTxes = true;
    LOCK(cs_vNodes);
    vNodes.push_back(&node);
    ClearInventory(&node);

    CTxMemPool &pool = mempool;
    pool.clear();

    TestMemPoolEntryHelper entry;

    CBasicKeyStore keystore;
    CCoinsView coinsDummy;
    CCoinsViewCache coins(&coinsDummy);
    std::vector<CMutableTransaction> dummyTransactions = SetupDummyInputs(keystore, coins);

    // Create a basic signed transactions and add them to the pool. We will use these transactions
    // to create the spend and respend transactions.
    CMutableTransaction t1;
    t1.nLockTime = 0;
    t1.vin.resize(1);
    t1.vin[0].prevout = COutPoint(dummyTransactions[0].GetIdem(), 0);
    t1.vin[0].amount = dummyTransactions[0].vout[0].nValue;
    t1.vout.resize(1);
    t1.vout[0].nValue = 50 * CENT;
    CKey key;
    key.MakeNewKey(true);
    keystore.AddKey(key);
    t1.vout[0].scriptPubKey << OP_DUP << OP_HASH160 << ToByteVector(key.GetPubKey().GetID()) << OP_EQUALVERIFY
                            << OP_CHECKSIG;

    CTransaction tx1(t1);
    {
        TransactionSignatureCreator tsc(&keystore, &tx1, 0, defaultSigHashType);
        const CScript &scriptPubKey = dummyTransactions[0].vout[0].scriptPubKey;
        CScript &scriptSigRes = t1.vin[0].scriptSig;
        bool worked = ProduceSignature(tsc, scriptPubKey, scriptSigRes);
        BOOST_CHECK(worked);
    }
    CTransaction tx1a(t1);
    pool.addUnchecked(entry.FromTx(tx1a));

    CMutableTransaction t2;
    t2.nLockTime = 0;
    t2.vin.resize(1);
    t2.vin[0].prevout = COutPoint(dummyTransactions[0].GetIdem(), 1);
    t2.vin[0].amount = dummyTransactions[0].vout[1].nValue;
    t2.vout.resize(1);
    t2.vout[0].nValue = 50 * CENT;
    key.MakeNewKey(true);
    keystore.AddKey(key);
    t2.vout[0].scriptPubKey << OP_DUP << OP_HASH160 << ToByteVector(key.GetPubKey().GetID()) << OP_EQUALVERIFY
                            << OP_CHECKSIG;
    CTransaction tx2(t2);
    {
        TransactionSignatureCreator tsc(&keystore, &tx2, 0, defaultSigHashType);
        const CScript &scriptPubKey = dummyTransactions[0].vout[1].scriptPubKey;
        CScript &scriptSigRes = t2.vin[0].scriptSig;
        bool worked = ProduceSignature(tsc, scriptPubKey, scriptSigRes);
        BOOST_CHECK(worked);
    }
    CTransaction tx2a(t2);
    pool.addUnchecked(entry.FromTx(tx2a));
    BOOST_CHECK(pool.size() == 2);


    // Create a spend of tx1's output.
    CMutableTransaction s1;
    s1.nLockTime = 0;
    s1.vin.resize(1);
    s1.vin[0].prevout = COutPoint(tx1a.GetIdem(), 0);
    s1.vin[0].amount = tx1a.vout[0].nValue;
    s1.vout.resize(1);
    s1.vout[0].nValue = 50 * CENT;
    key.MakeNewKey(true);
    keystore.AddKey(key);
    s1.vout[0].scriptPubKey << OP_DUP << OP_HASH160 << ToByteVector(key.GetPubKey().GetID()) << OP_EQUALVERIFY
                            << OP_CHECKSIG;

    CTransaction spend1(s1);
    {
        TransactionSignatureCreator tsc(&keystore, &spend1, 0, defaultSigHashType);
        const CScript &scriptPubKey = tx1a.vout[0].scriptPubKey;
        CScript &scriptSigRes = s1.vin[0].scriptSig;
        bool worked = ProduceSignature(tsc, scriptPubKey, scriptSigRes);
        BOOST_CHECK(worked);
    }
    CTransaction spend1a(s1);


    // Create a respend tx1a's output.
    CMutableTransaction s2;
    s2.nLockTime = 0;
    s2.vin.resize(1);
    s2.vin[0].prevout = COutPoint(tx1a.GetIdem(), 0);
    s2.vin[0].amount = tx1a.vout[0].nValue;
    s2.vout.resize(1);
    s2.vout[0].nValue = 50 * CENT;
    key.MakeNewKey(true);
    keystore.AddKey(key);
    s2.vout[0].scriptPubKey << OP_DUP << OP_HASH160 << ToByteVector(key.GetPubKey().GetID()) << OP_EQUALVERIFY
                            << OP_CHECKSIG;

    CTransaction spend2(s2);
    {
        TransactionSignatureCreator tsc(&keystore, &spend2, 0, defaultSigHashType);
        const CScript &scriptPubKey = tx1a.vout[0].scriptPubKey;
        CScript &scriptSigRes = s2.vin[0].scriptSig;
        bool worked = ProduceSignature(tsc, scriptPubKey, scriptSigRes);
        BOOST_CHECK(worked);

        { // Sanity check that the signature is actually correct
            TransactionSignatureChecker checker1(
                &spend2, 0, STANDARD_SCRIPT_VERIFY_FLAGS | SCRIPT_ENABLE_SIGHASH_FORKID);
            CValidationState empty;
            std::vector<CTxOut> fakeprevouts;
            fakeprevouts.resize(spend2.vin.size());
            ScriptImportedState sis1(&checker1, MakeTransactionRef(spend2), empty, fakeprevouts, 0);
            ScriptError_t error;
            if (!VerifyScript(scriptSigRes, scriptPubKey, STANDARD_SCRIPT_VERIFY_FLAGS | SCRIPT_ENABLE_SIGHASH_FORKID,
                    sis1, &error))
            {
                LOG(DSPROOF, "Sanity check signature failed due to: %s\n", ScriptErrorString(error));
                assert(0);
            }
        }
    }
    CTransaction spend2a(s2);

    // add a ds orphan for spend1a and spend2a
    ClearInventory(&node);
    DoubleSpendProof dsp_first;
    {
        READLOCK(pool.cs_txmempool);
        dsp_first = DoubleSpendProof::create(spend1a, spend2a, pool);
        {
            auto ref = MakeTransactionRef(spend2a);
            auto rc = dsp_first.validate(pool, ref);
            BOOST_CHECK(rc == DoubleSpendProof::Valid);
        }
    }

    int peerId = 1;
    pool.doubleSpendProofStorage()->addOrphan(dsp_first, peerId);

    // Check that the orphan is present and can be looked up correctly
    BOOST_CHECK(pool.doubleSpendProofStorage()->exists(dsp_first.GetHash()) == true);
    std::list<std::pair<int, int> > dsp_list1 =
        pool.doubleSpendProofStorage()->findOrphans(COutPoint(tx1a.GetIdem(), 0));
    BOOST_CHECK(dsp_list1.size() == 1);
    BOOST_CHECK_EQUAL(size_t(0), node.GetInventoryToSendSize());

    // Try looking up orphans that should not exist
    std::list<std::pair<int, int> > dsp_list2 =
        pool.doubleSpendProofStorage()->findOrphans(COutPoint(tx1a.GetIdem(), 1));
    BOOST_CHECK(dsp_list2.size() == 0);

    std::list<std::pair<int, int> > dsp_list3 =
        pool.doubleSpendProofStorage()->findOrphans(COutPoint(tx2a.GetIdem(), 0));
    BOOST_CHECK(dsp_list3.size() == 0);

    // do a check for respend to trigger the orphan code with spend1a. The orphan will be removed and the inv
    // for the dsproof should be broadcast.
    ClearInventory(&node);


    RespendDetector detector(pool, MakeTransactionRef(spend2a), {dummyaction});
    BOOST_CHECK_EQUAL(size_t(1), node.GetInventoryToSendSize());
    BOOST_CHECK(!node.vInventoryToSend.empty() && 7 == node.vInventoryToSend.at(0).type);
    std::list<std::pair<int, int> > dsp_list4 =
        pool.doubleSpendProofStorage()->findOrphans(COutPoint(tx1a.GetIdem(), 0));
    BOOST_CHECK(dsp_list4.size() == 0);


    // Check that orphan is removed when we add the dsproof again. This could happen
    // in a multi-threaded environment where both an orphan gets added just before we add
    // a fully validated proof (which happens after we've tried to reclaim prior orphans).
    int proofId = pool.doubleSpendProofStorage()->add(dsp_first).second;
    pool.doubleSpendProofStorage()->remove(proofId);
    BOOST_CHECK(pool.doubleSpendProofStorage()->exists(dsp_first.GetHash()) == false);

    pool.doubleSpendProofStorage()->addOrphan(dsp_first, peerId);
    proofId = pool.doubleSpendProofStorage()->add(dsp_first).second;
    BOOST_CHECK(pool.doubleSpendProofStorage()->orphanCount(proofId) == 0);

    // Cleanup
    vNodes.erase(vNodes.end() - 1);
}
BOOST_AUTO_TEST_SUITE_END();
