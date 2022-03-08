// Copyright (C) 2019-2020 Tom Zander <tomz@freedommail.ch>
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "DoubleSpendProof.h"
#include "hashwrapper.h"
#include "main.h"
#include "pubkey.h"
#include "script/interpreter.h"
#include "script/sign.h"
#include "script/standard.h"
#include "txmempool.h"
#include "validationinterface.h"

#include <stdexcept>

#ifdef ENABLE_WALLET
#include "wallet/db.h"
#include "wallet/wallet.h"
#include "wallet/walletdb.h"
#endif

#ifdef ANDROID // log sighash calculations
#include <android/log.h>
#define p(...) __android_log_print(ANDROID_LOG_DEBUG, "bu.sig", __VA_ARGS__)
#else
#define p(...)
// tinyformat::format(std::cout, __VA_ARGS__)
#endif

namespace
{
enum Scripts
{
    P2PKH
};

static bool IsPayToPubKeyHash(const CScript &script)
{
    txnouttype outtype = TX_NONSTANDARD;
    std::vector<CTxDestination> dests;
    int nReq = 0;
    if (!ExtractDestinations(script, outtype, dests, nReq))
        return false;
    if (outtype != TX_PUBKEYHASH || dests.size() != 1 || nReq != 1)
        return false;

    return true;
}

void getP2PKHSignature(const CScript &script, std::vector<uint8_t> &vchRet)
{
    auto scriptIter = script.begin();
    opcodetype type;
    script.GetOp(scriptIter, type, vchRet);
}

void hashTx(DoubleSpendProof::Spender &spender, const CTransaction &tx, int inputIndex)
{
    DbgAssert(!spender.pushData.empty(), return );
    DbgAssert(!spender.pushData.front().empty(), return );
    auto sigHashType = SigHashType(spender.pushData.front());
    if (!sigHashType.hasAnyoneCanPay())
    {
        spender.hashPrevOutputs = GetPrevoutHash(tx);
        spender.hashInAmounts = GetInputAmountHash(tx);
    }
    p("Hashing prevouts to: %s\n", spender.hashPrevOutputs.GetHex().c_str());
    p("Hashing input amounts to: %s\n", spender.hashInAmounts.GetHex().c_str());
    if (!sigHashType.hasAnyoneCanPay() && !sigHashType.hasSingle() && !sigHashType.hasNone())
    {
        spender.hashSequence = GetSequenceHash(tx);
        p("Hashing input sequence numbers to: %s\n", spender.hashSequence.GetHex().c_str());
    }
    if (!sigHashType.hasSingle() && !sigHashType.hasNone())
    {
        spender.hashOutputs = GetOutputsHash(tx);
        p("Hashing every output to: %s\n", spender.hashOutputs.GetHex().c_str());
    }
    else if (sigHashType.hasSingle() && (size_t)inputIndex < tx.vout.size())
    {
        CHashWriter ss(SER_GETHASH, 0);
        ss << tx.vout[inputIndex];
        spender.hashOutputs = ss.GetHash();
        p("Hashing just output %d to: %s\n", inputIndex, spender.hashOutputs.GetHex().c_str());
    }
}

class DSPSignatureChecker : public BaseSignatureChecker
{
public:
    DSPSignatureChecker(const DoubleSpendProof *proof, const DoubleSpendProof::Spender &spender, int64_t amount)
        : m_proof(proof), m_spender(spender), m_amount(amount)
    {
    }

    bool CheckSig(const std::vector<uint8_t> &vchSigIn,
        const std::vector<uint8_t> &vchPubKey,
        const CScript &scriptCode) const override
    {
        CPubKey pubkey(vchPubKey);
        if (!pubkey.IsValid())
            return false;

        std::vector<uint8_t> vchSig(vchSigIn);
        if (vchSig.empty())
            return false;
        vchSig.pop_back(); // drop the hashtype byte tacked on to the end of the signature

        p("DSP construct hash:\n");
        CHashWriter ss(SER_GETHASH, 0);
        ss << ((uint8_t)m_spender.txVersion) << m_spender.hashPrevOutputs << m_spender.hashInAmounts
           << m_spender.hashSequence;
        p("txversion: %x\n", m_spender.txVersion);
        p("prevouts: %s\n", m_spender.hashPrevOutputs.GetHex().c_str());
        p("input amounts: %s\n", m_spender.hashInAmounts.GetHex().c_str());
        p("input sequence numbers: %s\n", m_spender.hashSequence.GetHex().c_str());
        ss << m_proof->Outpoint();
        p("outpoint: %s\n", m_proof->Outpoint().GetHex().c_str());
        ss << static_cast<const CScriptBase &>(scriptCode);
        p("ScriptCode: %s\n", scriptCode.GetHex().c_str());
        ss << m_amount << m_spender.outSequence << m_spender.hashOutputs;
        p("Amount: %ld\n", (long int)m_amount);
        p("This input sequence: %d\n", m_spender.outSequence);
        p("hashOutputs: %s\n", m_spender.hashOutputs.GetHex().c_str());
        SigHashType sighashtype(m_spender.pushData.front());
        ss << m_spender.lockTime << sighashtype;
        p("Locktime: %d\n", m_spender.lockTime);
        p("sighashtype: %x\n", sighashtype.getRawSigHashType());
        p("Num bytes hashed: %d\n", ss.GetNumBytesHashed());
        const uint256 sighash = ss.GetHash();
        p("Final sighash is: %s\n", sighash.GetHex().c_str());

        return pubkey.VerifySchnorr(sighash, vchSig);
    }
    bool CheckLockTime(const CScriptNum &) const override { return true; }
    bool CheckSequence(const CScriptNum &) const override { return true; }
    const DoubleSpendProof *m_proof;
    const DoubleSpendProof::Spender &m_spender;
    const int64_t m_amount;
};
} // namespace

// static
DoubleSpendProof DoubleSpendProof::create(const CTransaction &t1, const CTransaction &t2, CTxMemPool &pool)
{
    AssertLockHeld(pool.cs_txmempool);

    if (t1.GetId() == t2.GetId())
        throw std::runtime_error("Can not create dsproof from identical transactions");

    DoubleSpendProof answer;
    Spender &s1 = answer.m_spender1;
    Spender &s2 = answer.m_spender2;

    size_t inputIndex1 = 0;
    size_t inputIndex2 = 0;
    for (; inputIndex1 < t1.vin.size(); ++inputIndex1)
    {
        const CTxIn &in1 = t1.vin.at(inputIndex1);
        for (inputIndex2 = 0; inputIndex2 < t2.vin.size(); ++inputIndex2)
        {
            const CTxIn &in2 = t2.vin.at(inputIndex2);
            if (in1.prevout == in2.prevout)
            {
                // Get the coin if it exists. Because this is a double spent coin the coin is likely spent and we
                // need to check the mempool to get the coin.
                const CCoinsViewMemPool viewMemPool(pcoinsTip, pool);
                Coin coin;
                if (!viewMemPool.GetCoin(in1.prevout, coin))
                    throw std::runtime_error(
                        strprintf("Coin was not found for double spend %s", in1.prevout.hash.ToString()));

                // Currently we only allow P2PKH
                if (!IsPayToPubKeyHash(coin.out.scriptPubKey))
                    throw std::runtime_error("Can not create dsproof: Transaction was not P2PKH");

                answer.m_prevOutpoint = in1.prevout;

                s1.outSequence = in1.nSequence;
                s2.outSequence = in2.nSequence;

                s1.pushData.resize(1);
                getP2PKHSignature(in1.scriptSig, s1.pushData.front());
                s2.pushData.resize(1);
                getP2PKHSignature(in2.scriptSig, s2.pushData.front());

                assert(!s1.pushData.empty()); // we resized it
                assert(!s2.pushData.empty()); // we resized it
                if (s1.pushData.front().empty() || s2.pushData.front().empty())
                    throw std::runtime_error("scriptSig has no signature");
                auto hashType = SigHashType(s1.pushData.front());
                if (!hashType.isBch())
                    throw std::runtime_error("Tx1 is not a Bitcoin Cash transaction");

                hashType = SigHashType(s2.pushData.front());
                if (!hashType.isBch())
                    throw std::runtime_error("Tx2 is not a Bitcoin Cash transaction");

                break;
            }
        }
    }

    if (answer.m_prevOutpoint.IsNull())
        throw std::runtime_error("Transactions do not double spend each other");

    s1.txVersion = t1.nVersion;
    s2.txVersion = t2.nVersion;
    s1.lockTime = t1.nLockTime;
    s2.lockTime = t2.nLockTime;

    hashTx(s1, t1, inputIndex1);
    hashTx(s2, t2, inputIndex2);

    // sort the spenders so the proof stays the same, independent of the order of tx seen first
    int diff = s1.hashOutputs.Compare(s2.hashOutputs);
    if (diff == 0)
        diff = s1.hashPrevOutputs.Compare(s2.hashPrevOutputs);
    if (diff > 0)
        std::swap(s1, s2);

    return answer;
}

DoubleSpendProof::DoubleSpendProof() {}
bool DoubleSpendProof::isEmpty() const { return m_prevOutpoint.IsNull(); }
DoubleSpendProof::Validity DoubleSpendProof::validate(const CTxMemPool &pool, const CTransactionRef ptx) const
{
    AssertLockHeld(pool.cs_txmempool);

    if (m_prevOutpoint.IsNull())
    {
        LOG(DSPROOF, "WARNING: Previous transaction id or or output index for dsproof is either null or invalid\n");
        return Invalid;
    }
    if (m_spender1.pushData.empty() || m_spender1.pushData.front().empty() || m_spender2.pushData.empty() ||
        m_spender2.pushData.front().empty())
    {
        LOG(DSPROOF, "WARNING: One or both signatures for dsproof are empty\n");
        return Invalid;
    }

    if (m_spender1 == m_spender2)
    {
        LOG(DSPROOF, "Warning:  Spenders in a dsproof must not be the same");
        return Invalid;
    }

    // check if ordering is proper. By convention, the first tx must have the smaller hash.
    int diff = m_spender1.hashOutputs.Compare(m_spender2.hashOutputs);
    if (diff == 0)
        diff = m_spender1.hashPrevOutputs.Compare(m_spender2.hashPrevOutputs);
    if (diff > 0)
    {
        LOG(DSPROOF, "WARNING: Transaction id ordering in dsproof is incorrect\n");
        return Invalid;
    }

    // Get the previous output we are spending.
    int64_t amount;
    CScript prevOutScript;
    {
        auto prev = pool._getTxIdx(m_prevOutpoint);
        if (prev.ptx.get())
        {
            amount = prev.GetValue();
            prevOutScript = prev.GetConstraintScript();
        }
        else // tx is not found in our mempool, look in the UTXO.
        {
            Coin coin;
            if (!pcoinsTip->GetCoin(m_prevOutpoint, coin))
            {
                /* if the output we spend is missing then either the tx just got mined
                 * or, more likely, our mempool just doesn't have it.
                 */
                return MissingUTXO;
            }
            amount = coin.GetValue();
            prevOutScript = coin.GetConstraintScript();
        }
    }

    /*
     * Find the matching transaction spending this. Possibly identical to one
     * of the sides of this DSP.
     * We need this because we want the public key that it contains.
     */
    CTransaction tx;
    if (ptx == nullptr)
    {
        auto it = pool.mapNextTx.find(m_prevOutpoint);
        if (it == pool.mapNextTx.end())
        {
            return MissingTransaction;
        }
        tx = *(it->second.ptx);
    }
    else
        tx = *ptx;

    /*
     * TomZ: At this point (2019-07) we only support P2PKH payments.
     *
     * Since we have an actually spending tx, we could trivially support various other
     * types of scripts because all we need to do is replace the signature from our 'tx'
     * with the one that comes from the DSP.
     */
    Scripts scriptType = P2PKH; // FUTURE: look at prevTx to find out script-type

    StackItem pubkeyStk;
    for (size_t i = 0; i < tx.vin.size(); ++i)
    {
        if (tx.vin[i].prevout == m_prevOutpoint)
        {
            // Found the input script we need!
            CScript inScript = tx.vin[i].scriptSig;
            auto scriptIter = inScript.begin();
            opcodetype type;
            if (!inScript.GetOp(scriptIter, type)) // P2PKH: first signature
            {
                LOG(DSPROOF, "WARNING: dsproof is invalid because GetOp() for signature failed\n");
                return Invalid;
            }
            if (!inScript.GetOp(scriptIter, type, pubkeyStk)) // then pubkey
            {
                LOG(DSPROOF, "WARNING: dsproof is invalid because GetOP() for pubkey failed\n");
                return Invalid;
            }
            break;
        }
    }

    // Nextchain
    if (!pubkeyStk.isVch())
    {
        LOG(DSPROOF, "WARNING: dsproof is invalid because pubkey is not a byte array\n");
        return Invalid;
    }
    VchType pubkey = pubkeyStk.asVch();

    if (pubkey.empty())
    {
        LOG(DSPROOF, "WARNING: dsproof is invalid because pubkey is empty\n");
        return Invalid;
    }

    CScript inScript;
    if (scriptType == P2PKH)
    {
        inScript << m_spender1.pushData.front();
        inScript << pubkey;
    }

    // DS proofs won't work for complex scripts (non P2PKH), which is good because we aren't storing the tx associated
    // with the Spender right now anyway.  So giving an empty tx and invalid input index to the verifier is ok,
    // since OP_PUSH_TX_DATA won't be used.
    CTransaction noTx;
    CTransactionRef noTxRef = MakeTransactionRef(noTx);

    DSPSignatureChecker checker1(this, m_spender1, amount);
    ScriptImportedState sis1(&checker1);
    ScriptError_t error;
    if (!VerifyScript(inScript, prevOutScript, checker1.flags(), sis1, &error))
    {
        LOG(DSPROOF, "DoubleSpendProof failed validating first tx due to %s\n", ScriptErrorString(error));
        return Invalid;
    }

    inScript.clear();
    if (scriptType == P2PKH)
    {
        inScript << m_spender2.pushData.front();
        inScript << pubkey;
    }
    DSPSignatureChecker checker2(this, m_spender2, amount);
    ScriptImportedState sis2(&checker2);
    if (!VerifyScript(inScript, prevOutScript, checker2.flags(), sis2, &error))
    {
        LOG(DSPROOF, "DoubleSpendProof failed validating second tx due to %s\n", ScriptErrorString(error));
        return Invalid;
    }
    return Valid;
}

void broadcastDspInv(const CTransactionRef &dspTx, const uint256 &hash, CTxMemPool::setEntries *setDescendants)
{
#ifdef ENABLE_WALLET
    // If this transaction is in the wallet then mark it as doublespent
    if (pwalletMain)
        pwalletMain->MarkDoubleSpent(dspTx->GetId());
#endif

    // Notify zmq
    GetMainSignals().SyncDoubleSpend(dspTx);
    // send INV to all peers
    CInv inv(MSG_DOUBLESPENDPROOF, hash);
    LOG(DSPROOF, "Broadcasting dsproof INV: %s\n", hash.ToString());

    LOCK(cs_vNodes);
    for (CNode *pnode : vNodes)
    {
        if (!pnode->fRelayTxes)
            continue;
        LOCK(pnode->cs_filter);
        if (pnode->pfilter)
        {
            if (setDescendants)
            {
                for (auto iter : *setDescendants)
                {
                    if (pnode->pfilter->IsRelevantAndUpdate(iter->GetSharedTx()))
                        pnode->PushInventory(inv);
                }
            }
            // For nodes that we sent this Tx before, send a proof.
            else if (pnode->pfilter->IsRelevantAndUpdate(dspTx))
                pnode->PushInventory(inv);
        }
        else
        {
            pnode->PushInventory(inv);
        }
    }
}

uint256 DoubleSpendProof::GetHash() const { return SerializeHash(*this); }
