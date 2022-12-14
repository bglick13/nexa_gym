// Copyright (c) 2019 Andrew Stone Consulting
// Copyright (c) 2021 Bitcoin Unlimited
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


// Counterparty and protocol discovery
#include <limits>
#include <queue>

#include "arith_uint256.h"
#include "capd.h"
#include "clientversion.h"
#include "dosman.h"
#include "init.h"
#include "net.h"
#include "serialize.h"
#include "streams.h"
#include "uint256.h"

#include "hashwrapper.h"
#include "httpserver.h"
#include "rpc/server.h"
#include "utilstrencodings.h"

#include <univalue.h>

const CapdMsgRef nullmsgref = CapdMsgRef();

uint64_t msgpoolMaxSize = CapdMsgPool::DEFAULT_MSG_POOL_MAX_SIZE;

const long int YEAR_OF_SECONDS = 31536000;
const uint64_t CAPD_MAX_INV_TO_SEND = 20000;
const uint64_t CAPD_MAX_MSG_TO_REQUEST = 10000;
const uint64_t CAPD_MAX_MSG_TO_SEND = 5000;

CapdMsgPool msgpool;
CapdProtocol capdProtocol(msgpool);

uint64_t MSG_LIFETIME_SEC = 60 * 10; // expected message lifetime in seconds
uint64_t NOMINAL_MSG_SIZE = 100; // A message of this size or less has no penalty

// Local message difficulty must be less than forwarded message difficulty
arith_uint256 MIN_FORWARD_MSG_DIFFICULTY("007fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
arith_uint256 MIN_LOCAL_MSG_DIFFICULTY("00ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");


PriorityType MIN_RELAY_PRIORITY = Priority(MIN_FORWARD_MSG_DIFFICULTY, NOMINAL_MSG_SIZE, 0);
PriorityType MIN_LOCAL_PRIORITY = Priority(MIN_LOCAL_MSG_DIFFICULTY, NOMINAL_MSG_SIZE, 0);

arith_uint256 MAX_UINT256 = ~arith_uint256();

/* When converting a double to a uint256, precision will be lost.  This is how much to keep.
   Basically, doubles are multiplied by this amount before being converted to a uint256.
   This means that this precision is often lost in related uint256 values because the algorithm
   looks something like this to avoid overflow:

   uintVal/CONVERSION_FRAC * (uint256) ((uint64_t) (doubleVal * CONVERSION_FRAC))
 */
const unsigned long int PRIORITY_CONVERSION_FRAC = 0x100000;

PriorityType Priority(uint32_t difficultyBits, size_t msgContentSize, uint64_t age)
{
    arith_uint256 difficulty;
    difficulty.SetCompact(difficultyBits);
    return Priority(difficulty, msgContentSize, age);
}

PriorityType Priority(arith_uint256 hashTarget, size_t msgContentSize, uint64_t age)
{
    double ret = MIN_LOCAL_MSG_DIFFICULTY.getdouble() / hashTarget.getdouble();

    // Calculate the size based penalty
    if (msgContentSize > NOMINAL_MSG_SIZE)
    {
        ret = (ret / msgContentSize) * NOMINAL_MSG_SIZE;
    }

    // Next subtract the creation time based penalty
    double penaltyPerSec = ret / MSG_LIFETIME_SEC;
    double totalPenalty = penaltyPerSec * age;

    ret -= totalPenalty;
    return ret;
}

arith_uint256 aPriorityToPowTarget(PriorityType priority, size_t msgContentSize)
{
    if (msgContentSize > NOMINAL_MSG_SIZE)
    {
        priority = (priority * msgContentSize) / NOMINAL_MSG_SIZE;
    }

    arith_uint256 ret;
    if (priority < 1.0)
    {
        PriorityType priInv = (((PriorityType)1) / priority) * PRIORITY_CONVERSION_FRAC;
        ret = MIN_LOCAL_MSG_DIFFICULTY / PRIORITY_CONVERSION_FRAC * arith_uint256((uint64_t)priInv);
    }
    else
    {
        arith_uint256 v;
        v.setdouble(priority * PRIORITY_CONVERSION_FRAC);
        ret = (MIN_LOCAL_MSG_DIFFICULTY / v) * PRIORITY_CONVERSION_FRAC;
    }

    return ret;
}

uint256 CapdMsg::CalcHash() const
{
    CDataStream serialized(SER_GETHASH, CLIENT_VERSION);
    serialized << *this;

    CSHA256 sha;
    sha.Write((unsigned char *)serialized.data(), serialized.size());
    unsigned char stage1[CSHA256::OUTPUT_SIZE];
    sha.Finalize(stage1);
    uint256 hash;
    CHash256 sha2;
    sha2.Write(stage1, sizeof(stage1));
    sha2.Write(&nonce[0], nonce.size());
    sha2.Finalize(hash.begin());
    cachedHash = hash;
    return hash;
}

PriorityType CapdMsg::Priority() const { return ::Priority(difficultyBits, data.size(), GetTime() - createTime); }
PriorityType CapdMsg::InitialPriority() const { return ::Priority(difficultyBits, data.size(), 0); }
bool CapdMsg::DoesPowMeetTarget() const { return GetPowTarget() > GetHash(); }
std::string CapdMsg::EncodeHex()
{
    CDataStream strmdata(SER_NETWORK, PROTOCOL_VERSION);
    strmdata << *this;
    return HexStr(strmdata.begin(), strmdata.end());
}


bool CapdMsg::checkNonce(unsigned char *stage1, const arith_uint256 &hashTarget)
{
    uint256 hash;
    CHash256 sha2;
    sha2.Write(stage1, CSHA256::OUTPUT_SIZE);
    sha2.Write(&nonce[0], nonce.size());
    sha2.Finalize(hash.begin());

    if (hashTarget > UintToArith256(hash))
        return true;
    return false;
}

bool CapdMsg::Solve(long int time)
{
    cachedHash = uint256(); // Clear the hash because we are changing the message
    if (time < YEAR_OF_SECONDS)
        createTime = GetTime() - time;
    else
        createTime = time;

    CDataStream serialized(SER_GETHASH, CLIENT_VERSION);
    serialized << *this;

    CSHA256 sha;
    sha.Write((unsigned char *)serialized.data(), serialized.size());
    unsigned char stage1[CSHA256::OUTPUT_SIZE];
    sha.Finalize(stage1);

    arith_uint256 hashTarget = arith_uint256().SetCompact(difficultyBits);

    bool solved = false;

    do
    {
        // Looking for the shortest solution means searching all possibilities of different nonce lengths.
        nonce.resize(1);
        uint64_t count = 0;
        while (count < 256)
        {
            nonce[0] = count & 255;
            if (checkNonce(stage1, hashTarget))
            {
                solved = true;
                break;
            }
            count++;
        }
        if (solved)
            break;

        count = 0;
        nonce.resize(2);
        while (count < 256 * 256)
        {
            nonce[0] = count & 255;
            nonce[1] = count >> 8;
            if (checkNonce(stage1, hashTarget))
            {
                solved = true;
                break;
            }
            count++;
        }
        if (solved)
            break;

        count = 0;
        nonce.resize(3);
        while (count < 256 * 256 * 256)
        {
            nonce[0] = count & 255;
            nonce[1] = count >> 8;
            nonce[2] = count >> 16;
            if (checkNonce(stage1, hashTarget))
            {
                solved = true;
                break;
            }
            count++;
        }

        count = 0;
        nonce.resize(4);
        while (count < 256UL * 256UL * 256UL * 256UL)
        {
            nonce[0] = count & 255;
            nonce[1] = count >> 8;
            nonce[2] = count >> 16;
            nonce[3] = count >> 24;
            if (checkNonce(stage1, hashTarget))
            {
                solved = true;
                break;
            }
            count++;
        }
    } while (0);

    return solved;
}

void CapdMsgPool::add(const CapdMsg &msg)
{
    CapdMsgRef m = std::make_shared<CapdMsg>(msg);
    add(m);
}

void CapdMsgPool::add(const CapdMsgRef &msg)
{
    bool broadcast = false;

    {
        WRITELOCK(csMsgPool);
        if (!msg->DoesPowMeetTarget())
        {
            LOG(CAPD, "Message POW inconsistent");
            throw CapdMsgPoolException("Message POW inconsistent");
        }

        if (msg->Priority() < _GetLocalPriority())
        {
            LOG(CAPD, "message priority %f below local priority %f", msg->Priority(), _GetLocalPriority());
            throw CapdMsgPoolException("Message POW too low");
        }

        // Insert the message if it doesn't already exist
        if (msgs.find(msg->GetHash()) == msgs.end())
        {
            // Free up some room
            _pare(msg->RamSize());
            msgs.insert(msg);
            size += msg->RamSize();
            broadcast = true;
        }
        else
        {
            LOG(CAPD, "message exists: %s", msg->GetHash().GetHex());
        }
    }

    if (broadcast)
    {
        auto *p = p2p; // Throw in a temp to avoid locking
        if (p)
            p->GossipMessage(msg);
    }
}

void CapdMsgPool::clear()
{
    WRITELOCK(csMsgPool);
    msgs.clear();
    size = 0;
}

void CapdMsg::SetPowTargetHarderThan(uint256 target)
{
    // Subtract 1 from the difficulty as a compact number.  By doing it this way we are certain
    // that the change isn't rounded away.
    uint32_t cInt = UintToArith256(target).GetCompact();
    uint32_t mantissa = (cInt & ((1 << 23) - 1));
    uint32_t randomReduction = (std::rand() % 0xfe) + 1;

    if (mantissa > randomReduction) // subtract a bit from the mantissa if we won't underflow
    {
        cInt -= randomReduction;
    }
    else // subtract one from the exponent and set the enough bits that its like 0x100 -> 0xff
    {
        cInt = (cInt - (1 << 25)) | 0xff;
    }

    arith_uint256 num;
    SetPowTarget(num.SetCompact(cInt));
}

void CapdMsg::SetPowTargetHarderThanPriority(PriorityType priority)
{
    SetPowTargetHarderThan(PriorityToPowTarget(priority, data.size()));
}

uint256 CapdMsgPool::_GetRelayPowTarget()
{
    static const uint256 minFwdDiffTgt = ArithToUint256(MIN_FORWARD_MSG_DIFFICULTY);

    if (size < maxSize * 8 / 10)
        return minFwdDiffTgt; // If the pool isn't filled up, return the minimum.

    auto &priorityIndexer = msgs.get<MsgPriorityTag>();

    unsigned int half = priorityIndexer.size() / 2;

    MsgIterByPriority i = priorityIndexer.begin();
    for (unsigned int j = 0; j < half; j++, i++)
    {
    } // essentially i += half;

    if (i == priorityIndexer.end())
        return minFwdDiffTgt;

    uint256 ret = (*i)->GetPowTarget();
    if (ret > minFwdDiffTgt)
        return minFwdDiffTgt;
    return ret;
}

uint256 CapdMsgPool::_GetLocalPowTarget()
{
    static const uint256 minLclDiffTgt = ArithToUint256(MIN_LOCAL_MSG_DIFFICULTY);

    if (size < maxSize * 8 / 10)
        return minLclDiffTgt; // If the pool isn't filled up, return the minimum.

    auto &priorityIndexer = msgs.get<MsgPriorityTag>();

    MsgIterByPriority i = priorityIndexer.begin();
    if (i == priorityIndexer.end())
        return minLclDiffTgt;

    uint256 ret = (*i)->GetPowTarget();
    if (ret > minLclDiffTgt)
        return minLclDiffTgt;
    return ret;
}

PriorityType CapdMsgPool::_GetRelayPriority()
{
    if (size < maxSize * 8 / 10)
        return MIN_RELAY_PRIORITY;

    auto &priorityIndexer = msgs.get<MsgPriorityTag>();

    unsigned int half = priorityIndexer.size() / 2;

    MsgIterByPriority i = priorityIndexer.begin();
    for (unsigned int j = 0; j < half; j++, i++)
    {
    } // essentially i += half;

    if (i == priorityIndexer.end())
        return MIN_RELAY_PRIORITY;

    PriorityType ret = (*i)->Priority();
    if (ret < MIN_RELAY_PRIORITY)
        return MIN_RELAY_PRIORITY;
    return ret;
}

PriorityType CapdMsgPool::_GetLocalPriority()
{
    if (size < maxSize * 8 / 10)
        return MIN_LOCAL_PRIORITY;

    auto &priorityIndexer = msgs.get<MsgPriorityTag>();

    MsgIterByPriority i = priorityIndexer.begin();
    if (i == priorityIndexer.end())
        return MIN_LOCAL_PRIORITY;

    PriorityType ret = (*i)->Priority();
    if (ret < MIN_LOCAL_PRIORITY)
        return MIN_LOCAL_PRIORITY;
    return ret;
}

PriorityType CapdMsgPool::_GetHighestPriority()
{
    auto &priorityIndexer = msgs.get<MsgPriorityTag>();

    MsgIterByPriority i = priorityIndexer.begin();
    if (i == priorityIndexer.end())
        return MIN_RELAY_PRIORITY;

    MsgIterByPriority last = priorityIndexer.end();
    last--;

    PriorityType ret = (*last)->Priority();
    if (ret < MIN_RELAY_PRIORITY)
        return MIN_RELAY_PRIORITY;
    return ret;
}


void CapdMsgPool::_pare(int len)
{
    len -= maxSize - size; // We already have this amount available
    auto &priorityIndexer = msgs.get<MsgPriorityTag>();
    MsgIterByPriority i = priorityIndexer.begin();
    auto end = priorityIndexer.end();

    while ((len > 0) && (i != end))
    {
        auto txSize = (*i)->RamSize();
        len -= txSize;
        size -= txSize;

        auto j = i; // Advance before erase
        i++;
        priorityIndexer.erase(j);
    }
}


CapdMsgRef CapdMsgPool::find(const uint256 &hash) const
{
    READLOCK(csMsgPool);
    MsgIter i = msgs.find(hash);
    if (i == msgs.end())
        return nullmsgref;
    return *i;
}

std::vector<CapdMsgRef> CapdMsgPool::find(const std::vector<unsigned char> &v) const
{
    READLOCK(csMsgPool);
    if (v.size() == 2)
    {
        auto &indexer = msgs.get<MsgLookup2>();
        std::array<unsigned char, 2> srch = {v[0], v[1]};
        MessageContainer::index<MsgLookup2>::type::iterator it = indexer.find(srch);

        std::vector<CapdMsgRef> ret;
        for (; it != indexer.end(); it++)
        {
            if (!(*it)->matches(srch))
                break;
            ret.push_back(*it);
        }
        return ret;
    }

    if (v.size() == 4)
    {
        auto &indexer = msgs.get<MsgLookup4>();
        std::array<unsigned char, 4> srch = {v[0], v[1], v[2], v[3]};
        MessageContainer::index<MsgLookup4>::type::iterator it = indexer.find(srch);

        std::vector<CapdMsgRef> ret;
        for (; it != indexer.end(); it++)
        {
            if (!(*it)->matches(srch))
                break;
            ret.push_back(*it);
        }
        return ret;
    }

    if (v.size() == 8)
    {
        auto &indexer = msgs.get<MsgLookup8>();
        std::array<unsigned char, 8> srch;
        for (auto i = 0; i < 8; i++)
            srch[i] = v[i];
        MessageContainer::index<MsgLookup8>::type::iterator it = indexer.find(srch);

        std::vector<CapdMsgRef> ret;
        for (; it != indexer.end(); it++)
        {
            if (!(*it)->matches(srch))
                break;
            ret.push_back(*it);
        }
        return ret;
    }

    if (v.size() == 16)
    {
        auto &indexer = msgs.get<MsgLookup16>();
        std::array<unsigned char, 16> srch;
        for (auto i = 0; i < 16; i++)
            srch[i] = v[i];
        MessageContainer::index<MsgLookup16>::type::iterator it = indexer.find(srch);

        std::vector<CapdMsgRef> ret;
        for (; it != indexer.end(); it++)
        {
            if (!(*it)->matches(srch))
                break;
            ret.push_back(*it);
        }
        return ret;
    }

    return std::vector<CapdMsgRef>();
}

static const uint64_t MSGPOOL_DUMP_VERSION = 1;
bool CapdMsgPool::LoadMsgPool(void)
{
    FILE *fileMsgpool = fopen((GetDataDir() / "msgpool.dat").string().c_str(), "rb");
    if (!fileMsgpool)
    {
        LOGA("Failed to open msgpool file from disk. Continuing anyway.\n");
        return false;
    }
    CAutoFile file(fileMsgpool, SER_DISK, CLIENT_VERSION);
    if (file.IsNull())
    {
        LOGA("Failed to open msgpool file from disk. Continuing anyway.\n");
        return false;
    }

    int64_t count = 0;
    try
    {
        uint64_t version;
        file >> version;
        if (version != MSGPOOL_DUMP_VERSION)
        {
            return false;
        }
        uint64_t num;
        file >> num;
        WRITELOCK(csMsgPool);
        while (num--)
        {
            CapdMsg msg;
            file >> msg;
            CapdMsgRef ref = std::make_shared<CapdMsg>(msg);
            {
                msgs.insert(ref);
                size += ref->RamSize();
                ++count;
            }

            if (ShutdownRequested())
                return false;
        }
    }
    catch (const std::exception &e)
    {
        LOGA("Failed to deserialize msgpool data on disk: %s. Continuing anyway.\n", e.what());
        return false;
    }

    LOGA("Imported msgpool messages from disk: %i successes\n", count);
    return true;
}

bool CapdMsgPool::DumpMsgPool(void)
{
    int64_t start = GetStopwatchMicros();

    READLOCK(csMsgPool);
    auto &priorityIndexer = msgs.get<MsgPriorityTag>();

    int64_t mid = GetStopwatchMicros();

    try
    {
        FILE *fileMsgpool = fopen((GetDataDir() / "msgpool.dat.new").string().c_str(), "wb");
        if (!fileMsgpool)
        {
            LOGA("Could not dump txpool, failed to open the msgpool file from disk. Continuing anyway.\n");
            return false;
        }

        CAutoFile file(fileMsgpool, SER_DISK, CLIENT_VERSION);
        uint64_t version = MSGPOOL_DUMP_VERSION;
        file << version;

        file << (uint64_t)msgs.size();

        MsgIterByPriority i = priorityIndexer.begin();
        for (unsigned int j = 0; i != priorityIndexer.end(); j++, i++)
        {
            file << *((*i).get());
        }

        FileCommit(file.Get());
        file.fclose();
        RenameOver(GetDataDir() / "msgpool.dat.new", GetDataDir() / "msgpool.dat");
        int64_t last = GetStopwatchMicros();
        LOGA("Dumped msgpool: %gs to copy, %gs to dump\n", (mid - start) * 0.000001, (last - mid) * 0.000001);
    }
    catch (const std::exception &e)
    {
        LOGA("Failed to dump msgpool: %s. Continuing anyway.\n", e.what());
        return false;
    }
    return true;
}

void CapdMsgPool::_DbgDump()
{
    auto &priorityIndexer = msgs.get<MsgPriorityTag>();

    MsgIterByPriority i = priorityIndexer.begin();
    for (unsigned int j = 0; i != priorityIndexer.end(); j++, i++)
    {
        printf("%4d: priority:%f - difficulty:%s -- %s %.8s\n", j, (*i)->Priority(),
            (*i)->GetPowTarget().GetHex().c_str(), (*i)->GetHash().GetHex().c_str(), &(*i)->data[0]);
    }

    printf("relay: %s, local: %s\n", _GetRelayPowTarget().GetHex().c_str(), _GetLocalPowTarget().GetHex().c_str());
}


bool CapdProtocol::HandleCapdMessage(CNode *pfrom,
    std::string &command,
    CDataStream &vRecv,
    int64_t stopwatchTimeReceived)
{
    LOG(CAPD, "Capd: process message %s from %s\n", command.c_str(), pfrom->GetLogName());
    DbgAssert(pool != nullptr, return false);
    CapdNode *cn = pfrom->capd;
    if (!pfrom->IsCapdEnabled())
    {
        LOG(CAPD, "Capd: received message but not enabled on node %s\n", pfrom->GetLogName());
        return false;
    }
    if (!cn)
    {
        LOG(CAPD, "Capd: received message but no processing object on node %s\n", pfrom->GetLogName());
        return false;
    }

    if (command == NetMsgType::CAPDINFO)
    {
        double localPri;
        double relayPri;
        double highestPri;
        vRecv >> localPri >> relayPri >> highestPri;
        cn->sendPriority = relayPri;
    }
    else if (command == NetMsgType::CAPDGETINFO)
    {
        pfrom->PushMessage(
            NetMsgType::CAPDINFO, msgpool.GetLocalPriority(), msgpool.GetRelayPriority(), msgpool.GetHighestPriority());
    }
    else if (command == NetMsgType::CAPDQUERY)
    {
        uint32_t cookie;
        uint8_t type;
        uint32_t quantity;
        uint32_t start;
        std::vector<unsigned char> content;
        vRecv >> cookie >> type >> start >> quantity >> content;

        bool notify = false;
        if ((type & CAPD_QUERY_NOTIFY) > 0)
        {
            notify = true;
            type = type & ~CAPD_QUERY_NOTIFY;
        }

        auto sz = content.size();
        if ((sz == 2) || (sz == 4) || (sz == 8) || (sz == 16))
        {
            if (notify)
                cn->installNotification(cookie, quantity, type, content);

            std::vector<CapdMsgRef> msgs = msgpool.find(content);
            if (type == CAPD_QUERY_TYPE_MSG)
            {
                quantity = std::min((int)CAPD_QUERY_MAX_MSGS, (int)quantity);
                pfrom->PushMessage(NetMsgType::CAPDQUERYREPLY, cookie, type, (uint32_t)msgs.size(),
                    PtrVectorSpan<CapdMsgRef>(msgs, start, quantity));
            }
            if (type == CAPD_QUERY_TYPE_MSG_HASH)
            {
                int qty = std::min(
                    std::min((int)CAPD_QUERY_MAX_INVS + start, (int)start + quantity), (int)msgs.size() - start);
                std::vector<uint256> hashes;
                for (int i = start; i < qty; i++)
                {
                    hashes.push_back(msgs[i]->GetHash());
                }
                pfrom->PushMessage(NetMsgType::CAPDQUERYREPLY, cookie, type, (uint32_t)msgs.size(), hashes);
            }
        }
        else
        {
            uint8_t error = sz;
            pfrom->PushMessage(NetMsgType::CAPDQUERYREPLY, cookie, (uint8_t)CAPD_QUERY_TYPE_ERROR, error);
        }
    }
    else if (command == NetMsgType::CAPDREMOVENOTIFY)
    {
        uint32_t cookie;
        vRecv >> cookie;
        cn->removeNotification(cookie);
    }
    else if (command == NetMsgType::CAPDQUERYREPLY)
    {
        // Ignore, I didn't query so this must be accidental
    }
    else if (command == NetMsgType::CAPDINV)
    {
        std::vector<uint256> vInv;
        int objtype = ReadCompactSize(vRecv);
        if (objtype != CAPD_MSG_TYPE) // unknown object type
        {
            return error(CAPD, "Received INV with unknown type %d\n", objtype);
        }
        vRecv >> vInv;
        LOG(CAPD, "Capd: Received %d message INVs", vInv.size());
        if (vInv.size() > CAPD_MAX_INV_TO_SEND)
        {
            dosMan.Misbehaving(pfrom, 20);
            return error(CAPD, "Received message with too many (%d) INVs\n", vInv.size());
        }
        for (auto inv : vInv)
        {
            if (pool->find(inv) == nullptr)
            {
                LOG(CAPD, "received INV %s", inv.GetHex());
                cn->getData(CInv(objtype, inv));
            }
            else
            {
                LOG(CAPD, "repeat INV %s", inv.GetHex());
            }
        }
    }
    else if (command == NetMsgType::CAPDGETMSG)
    {
        std::vector<uint256> msgIds;
        vRecv >> cn->sendPriority;
        vRecv >> msgIds;
        LOG(CAPD, "Capd: Received %d message requests", msgIds.size());
        if (msgIds.size() > CAPD_MAX_MSG_TO_REQUEST)
        {
            dosMan.Misbehaving(pfrom, 20);
            return error(CAPD, "Capd drop: Received message with too many (%d) capd message requests\n", msgIds.size());
        }

        for (auto id : msgIds)
        {
            LOG(CAPD, "received GetMsg %s", id.GetHex());
            auto msg = pool->find(id);
            if (msg == nullptr)
            {
                LOG(CAPD, "Capd drop: requested unknown message\n");
                continue;
            }
            if (msg->Priority() < cn->sendPriority)
            {
                LOG(CAPD, "Capd drop: message priority %f below destination relay priority %f\n", msg->Priority(),
                    cn->sendPriority);
                continue;
            }

            cn->sendMsg(msg);
        }
    }
    else if (command == NetMsgType::CAPDMSG)
    {
        std::vector<CapdMsg> msgs; // TODO deserialize as CapdMsgRefs
        std::vector<CapdMsgRef> goodMsgs;
        vRecv >> msgs;
        LOG(CAPD, "Capd: Received %d messages", msgs.size());
        for (const CapdMsg &msg : msgs)
        {
            auto msgRef = MakeMsgRef(CapdMsg(msg));
            uint256 hash = msgRef->GetHash();
            LOG(CAPD, "received Msg %s\n", hash.GetHex());
            PriorityType priority = msgRef->Priority();
            LOG(CAPD, "Msg priority %f\n", priority);
            if (priority < cn->receivePriority)
            {
                dosMan.Misbehaving(pfrom, 1);
                LOG(CAPD, "Capd drop: message %s priority below minimum for node %s: %f %f\n",
                    msgRef->GetHash().GetHex(), pfrom->GetLogName(), priority, cn->receivePriority);
                continue;
            }

            try
            {
                pool->add(msgRef); // the pool will relay to P2P nodes if the message is viable
                goodMsgs.push_back(msgRef);
            }
            catch (CapdMsgPoolException &e)
            {
                // messsage was too low priority, drop it
                LOG(CAPD, "Capd drop: message priority too low to add");
            }
        }

        // Grab a copy of capd CNodes and add a ref so they can't be deleted
        std::vector<CNode *> capdNodes;
        {
            LOCK(cs_vNodes);
            for (CNode *pnode : vNodes)
            {
                if (pnode->isCapdEnabled && pnode->capd)
                {
                    capdNodes.push_back(pnode);
                    pnode->AddRef();
                }
            }
        }

        // Now see if we need to relay this message to a query peer
        for (const CapdMsgRef &msgRef : goodMsgs)
        {
            for (CNode *pnode : capdNodes)
            {
                pnode->capd->checkNotification(msgRef);
            }
        }

        // Release our refs to these nodes
        for (CNode *pnode : capdNodes)
            pnode->Release();
    }
    else
    {
        // TODO: Something more than ignore if I don't understand the capd message
    }
    return true;
}

void CapdProtocol::GossipMessage(const CapdMsgRef &msg)
{
    LOCK(csCapdProtocol);
    relayInv.push_back(std::pair<uint256, PriorityType>(msg->GetHash(), msg->Priority()));
}

void CapdProtocol::FlushGossipMessagesToNodes()
{
    {
        LOCK(csCapdProtocol);
        if (relayInv.empty())
            return;
    }

    // Grab a copy of capd CNodes and add a ref so they can't be deleted
    std::vector<CNode *> capdNodes;
    {
        LOCK(cs_vNodes);
        for (CNode *pnode : vNodes)
        {
            if (pnode->isCapdEnabled && pnode->capd)
            {
                capdNodes.push_back(pnode);
                pnode->AddRef();
            }
        }
    }

    {
        LOCK(csCapdProtocol);
        for (CNode *pnode : capdNodes)
        {
            CapdNode *cn = pnode->capd;
            if (!pnode->IsCapdEnabled() || !cn) // Weird, should not have been added to the list
            {
                LOG(CAPD, "Not relaying to non-capd node %s\n", pnode->GetLogName());
                continue;
            }

            for (auto &item : relayInv)
            {
                if (item.second >= cn->sendPriority)
                    cn->invMsg(item.first);
                else
                {
                    LOG(CAPD, "Not relaying %s to node %s (priority %f < %f)\n", item.first.GetHex(),
                        pnode->GetLogName(), item.second, cn->sendPriority);
                }
            }
        }
        relayInv.clear();
    }
    // A cs_vNodes lock is not required here when releasing refs for two reasons: one, this only decrements
    // an atomic counter, and two, the counter will always be > 0 at this point, so we don't have to worry
    // that a pnode could be disconnected and no longer exist before the decrement takes place.
    for (CNode *pnode : capdNodes)
    {
        pnode->Release();
    }
}

void CapdNode::clear()
{
    LOCK(csCapdNode);
    filterInventoryKnown.reset();
    invMsgs.clear();
    requestMsgs.clear();
    sendMsgs.clear();
    notifications.clear();
}

bool CapdNode::FlushMessages()
{
    LOCK(csCapdNode);
    DbgAssert(node, return false);

    if (!invMsgs.empty())
    {
        for (auto i : invMsgs)
        {
            LOG(CAPD, "flush INV for %s\n", i.GetHex());
        }

        unsigned int offset = 0;
        while (offset < invMsgs.size())
        {
            node->PushMessage(NetMsgType::CAPDINV, CompactSerializer(CapdProtocol::CAPD_MSG_TYPE),
                VectorSpan<uint256>(invMsgs, offset, CAPD_MAX_INV_TO_SEND));
            offset += CAPD_MAX_INV_TO_SEND;
        }

        invMsgs.clear();
    }

    if (!requestMsgs.empty())
    {
        unsigned int offset = 0;
        do
        {
            node->PushMessage(NetMsgType::CAPDGETMSG, msgpool.GetRelayPriority(),
                VectorSpan<uint256>(requestMsgs, offset, CAPD_MAX_MSG_TO_REQUEST));
            offset += CAPD_MAX_MSG_TO_REQUEST;
        } while (offset < requestMsgs.size());

        requestMsgs.clear();
    }

    if (!sendMsgs.empty())
    {
        unsigned int offset = 0;
        std::vector<CapdMsg> sm; // TODO figure out shared_ptr serialization
        for (auto m : sendMsgs)
            sm.push_back(*m);
        do
        {
            node->PushMessage(NetMsgType::CAPDMSG, VectorSpan<CapdMsg>(sm, offset, CAPD_MAX_MSG_TO_SEND));
            offset += CAPD_MAX_MSG_TO_SEND;
        } while (offset < sm.size());

        sendMsgs.clear();
    }

    return true;
}

void CapdNode::installNotification(uint32_t cookie,
    uint32_t maxQuantityPerMessage,
    uint8_t type,
    const std::vector<unsigned char> &content)
{
    LOCK(csCapdNode);
    notifications[content] = {cookie, maxQuantityPerMessage, type};
}


void CapdNode::removeNotification(uint32_t cookie)
{
    LOCK(csCapdNode);
    auto it = notifications.begin();
    while (it != notifications.end())
    {
        if (it->second.cookie == cookie)
            it = notifications.erase(it);
        else
            ++it;
    }
}

void CapdNode::checkNotification(const CapdMsgRef msg)
{
    if (msg == nullptr)
        return;
    LOCK(csCapdNode);
    if (node == nullptr)
        return; // In case the node has disconnected
    auto len = msg->data.size();

    constexpr size_t srchSizes[] = {2, 4, 8, 16};
    for (auto srch : srchSizes)
    {
        if (len >= srch)
        {
            auto elems = notifications.equal_range(std::vector<uint8_t>(msg->data.begin(), msg->data.begin() + srch));
            for (auto it = elems.first; it != elems.second; ++it)
            {
                const CapdNode::NotificationInfo &ni = it->second;
                if (ni.type == CapdProtocol::CAPD_QUERY_TYPE_MSG)
                {
                    node->PushMessage(NetMsgType::CAPDQUERYREPLY, ni.cookie, ni.type, 1, PtrVectorOf1<CapdMsgRef>(msg));
                }
                if (ni.type == CapdProtocol::CAPD_QUERY_TYPE_MSG_HASH)
                {
                    uint256 hash = msg->GetHash();
                    node->PushMessage(
                        NetMsgType::CAPDQUERYREPLY, ni.cookie, ni.type, 1, PtrVectorOf1<uint256 *>(&hash));
                }
            }
        }
    }
}


//  Connect via JSON HTTP

enum RetFormat
{
    RF_UNDEF,
    RF_BINARY,
    RF_HEX,
    RF_JSON,
};

static const struct
{
    enum RetFormat rf;
    const char *name;
} rf_names[] = {
    {RF_UNDEF, ""},
    {RF_BINARY, "bin"},
    {RF_HEX, "hex"},
    {RF_JSON, "json"},
};

static bool RETERR(HTTPRequest *req, enum HTTPStatusCode status, const std::string &message)
{
    req->WriteHeader("Content-Type", "text/plain");
    req->WriteReply(status, message + "\r\n");
    return false;
}

static enum RetFormat ParseDataFormat(std::string &param, const std::string &strReq)
{
    const std::string::size_type pos = strReq.rfind('.');
    if (pos == std::string::npos)
    {
        param = strReq;
        return rf_names[0].rf;
    }

    param = strReq.substr(0, pos);
    const std::string suff(strReq, pos + 1);

    for (unsigned int i = 0; i < ARRAYLEN(rf_names); i++)
        if (suff == rf_names[i].name)
            return rf_names[i].rf;

    /* If no suffix is found, return original string.  */
    param = strReq;
    return rf_names[0].rf;
}

static std::string AvailableDataFormatsString()
{
    std::string formats = "";
    for (unsigned int i = 0; i < ARRAYLEN(rf_names); i++)
        if (strlen(rf_names[i].name) > 0)
        {
            formats.append(".");
            formats.append(rf_names[i].name);
            formats.append(", ");
        }

    if (formats.length() > 0)
        return formats.substr(0, formats.length() - 2);

    return formats;
}

static bool CheckWarmup(HTTPRequest *req)
{
    std::string statusmessage;
    if (RPCIsInWarmup(&statusmessage))
        return RETERR(req, HTTP_SERVICE_UNAVAILABLE, "Service temporarily unavailable: " + statusmessage);
    return true;
}

// http://<IP>/capd/get/<data>
static bool capdHttpGet(HTTPRequest *req, const std::string &strURIPart)
{
    if (!CheckWarmup(req))
        return false;
    std::string hashStr;
    const RetFormat rf = ParseDataFormat(hashStr, strURIPart);

    std::vector<unsigned char> prefix;
    if (!IsHex(hashStr))
        return RETERR(req, HTTP_BAD_REQUEST, "Invalid hex string: " + hashStr);
    prefix = ParseHex(hashStr);

    std::vector<unsigned char> data = prefix;


    switch (rf)
    {
    case RF_BINARY:
    {
        std::string binaryTx(data.begin(), data.end());
        req->WriteHeader("Content-Type", "application/octet-stream");
        req->WriteReply(HTTP_OK, binaryTx);
        return true;
    }

    case RF_HEX:
    {
        std::string strHex = HexStr(data.begin(), data.end()) + "\n";
        req->WriteHeader("Content-Type", "text/plain");
        req->WriteReply(HTTP_OK, strHex);
        return true;
    }

    case RF_JSON:
    {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("search", HexStr(data.begin(), data.end()));
        std::string strJSON = obj.write() + "\n";
        req->WriteHeader("Content-Type", "application/json");
        req->WriteReply(HTTP_OK, strJSON);
        return true;
    }

    default:
    {
        return RETERR(req, HTTP_NOT_FOUND, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }

    // not reached
    return true; // continue to process further HTTP reqs on this cxn
}

static bool capdHttpSend(HTTPRequest *req, const std::string &strURIPart)
{
    if (!CheckWarmup(req))
        return false;
    std::string result = "OK";

    if (!IsHex(strURIPart))
        return RETERR(req, HTTP_BAD_REQUEST, "Invalid hex string: " + strURIPart);
    std::vector<unsigned char> msgData = ParseHex(strURIPart);
    CapdMsgRef msg = MakeMsgRef(CapdMsg(msgData));
    if (!msg->DoesPowMeetTarget())
    {
        result = "inconsistent message, incorrect proof-of-work";
    }
    else
    {
        try
        {
            msgpool.add(msg);
        }
        catch (CapdMsgPoolException &e)
        {
            result = e.what();
        }
    }

    req->WriteHeader("Content-Type", "text/plain");
    req->WriteReply(HTTP_OK, result);
    return true;
}


static const struct
{
    const char *prefix;
    bool (*handler)(HTTPRequest *req, const std::string &strReq);
} uri_prefixes[] = {
    {"/capd/get/", capdHttpGet},
    {"/capd/send/", capdHttpSend},
};

bool StartCapd()
{
    /* CAPD HTTPD handler currently off
    for (unsigned int i = 0; i < ARRAYLEN(uri_prefixes); i++)
        RegisterHTTPHandler(uri_prefixes[i].prefix, false, uri_prefixes[i].handler);
    */

    return true;
}

void InterruptCapd() {}
void StopCapd()
{
    for (unsigned int i = 0; i < ARRAYLEN(uri_prefixes); i++)
        UnregisterHTTPHandler(uri_prefixes[i].prefix, false);
}


std::string CapdMsgPoolSizeValidator(const uint64_t &value, uint64_t *item, bool validate)
{
    if (validate)
    {
        return std::string();
    }
    else
    {
        msgpool.SetMaxSize(msgpoolMaxSize);
    }
    return std::string();
}
