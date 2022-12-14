// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2015-2021 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef NEXA_NET_H
#define NEXA_NET_H

#include "banentry.h"
#include "blockrelay/compactblock.h"
#include "bloom.h"
#include "chainparams.h"
#include "compat.h"
#include "extversionmessage.h"
#include "fastfilter.h"
#include "fs.h"
#include "hashwrapper.h"
#include "iblt.h"
#include "limitedmap.h"
#include "netbase.h"
#include "policy/mempool.h"
#include "primitives/block.h"
#include "protocol.h"
#include "random.h"
#include "stat.h"
#include "streams.h"
#include "sync.h"
#include "threadgroup.h"
#include "uint256.h"
#include "unlimited.h"
#include "util.h" // FIXME: reduce scope

#include <atomic>
#include <deque>
#include <stdint.h>

#ifndef WIN32
#include <arpa/inet.h>
#endif

#include <boost/signals2/signal.hpp>


extern CTweak<uint32_t> netMagic;
extern CChain chainActive;
static CMessageHeader::MessageStartChars netOverride;
class CAddrMan;
class CSubNet;
class CNode;
class CNodeRef;
class CNetMessage;
class CapdNode;

namespace boost
{
class thread_group;
} // namespace boost

extern CCriticalSection cs_priorityRecvQ;
extern CCriticalSection cs_prioritySendQ;
extern CTweak<unsigned int> numMsgHandlerThreads;
extern std::deque<std::pair<CNodeRef, CNetMessage> > vPriorityRecvQ;
extern std::deque<CNodeRef> vPrioritySendQ;
extern std::atomic<bool> fPriorityRecvMsg;
extern std::atomic<bool> fPrioritySendMsg;

/** Time between pings automatically sent out for latency probing and keepalive (in seconds). */
static const int PING_INTERVAL = 2 * 60;
/** Time after which to disconnect, after waiting for a ping response (or inactivity). */
static const int TIMEOUT_INTERVAL = 20 * 60;
/** Run the feeler connection loop once every 2 minutes or 120 seconds. **/
static const int FEELER_INTERVAL = 120;
/** The maximum number of entries in an 'inv' protocol message */
static const unsigned int MAX_INV_SZ = 50000;
/** The maximum number of new addresses to accumulate before announcing. */
static const unsigned int MAX_ADDR_TO_SEND = 1000;
/** The maximum # of bytes to receive at once */
static const int64_t MAX_RECV_CHUNK = 256 * 1024;
/** -listen default */
static const bool DEFAULT_LISTEN = true;
/** -upnp default */
#ifdef USE_UPNP
static const bool DEFAULT_UPNP = USE_UPNP;
#else
static const bool DEFAULT_UPNP = false;
#endif
/** The maximum number of peer connections to maintain. */
static const unsigned int DEFAULT_MAX_PEER_CONNECTIONS = 125;
/** BU: The maximum number of outbound peer connections */
static const unsigned int DEFAULT_MAX_OUTBOUND_CONNECTIONS = 16;
/** Limits number of IPs learned from a DNS seed */
static const unsigned int MAX_DNS_SEEDED_IPS = 256;
/** BU: The daily maximum disconnects while searching for xthin nodes to connect */
static const unsigned int MAX_DISCONNECTS = 200;
/** The default for -maxuploadtarget. 0 = Unlimited */
static const uint64_t DEFAULT_MAX_UPLOAD_TARGET = 0;
/** Default for blocks only*/
static const bool DEFAULT_BLOCKSONLY = false;
/** Default for Extversion */
static const bool DEFAULT_USE_EXTVERSION = true;

/** Internal constant that indicates we have no common graphene versions. */
const uint64_t GRAPHENE_NO_VERSION_SUPPORTED = 0xfffffff;

// BITCOINUNLIMITED START
static const bool DEFAULT_FORCEBITNODES = false;
// BITCOINUNLIMITED END

static const bool DEFAULT_FORCEDNSSEED = false;
static const size_t DEFAULT_MAXRECEIVEBUFFER = 10 * 1000;
static const size_t DEFAULT_MAXSENDBUFFER = 10 * 1000;

unsigned int ReceiveFloodSize();
unsigned int SendBufferSize();

// Node IDs are currently signed but only values greater than zero are returned.  Zero or negative can be used as a
// sentinel value.
typedef int NodeId;

void AddOneShot(const std::string &strDest);
// Find a node by name.  Returns a null ref if no node found
CNodeRef FindNodeRef(const std::string &addrName);
// Find a node by id.  Returns a null ref if no node found
CNodeRef FindNodeRef(const NodeId id);
// Find a node by ip.  Returns a null ref if no node found
CNodeRef FindNodeRef(const CNetAddr &ip);
int DisconnectSubNetNodes(const CSubNet &subNet);
bool OpenNetworkConnection(const CAddress &addrConnect,
    bool fCountFailure,
    CSemaphoreGrant *grantOutbound = nullptr,
    const char *strDest = nullptr,
    bool fOneShot = false,
    bool fFeeler = false);
void MapPort(bool fUseUPnP);
unsigned short GetListenPort();
bool BindListenPort(const CService &bindAddr, std::string &strError, bool fWhitelisted = false);
void StartNode();
bool StopNode();

struct CombinerAll
{
    typedef bool result_type;

    template <typename I>
    bool operator()(I first, I last) const
    {
        while (first != last)
        {
            if (!(*first))
                return false;
            ++first;
        }
        return true;
    }
};

// Signals for message handling
struct CNodeSignals
{
    boost::signals2::signal<int()> GetHeight;
    boost::signals2::signal<bool(CNode *), CombinerAll> ProcessMessages;
    boost::signals2::signal<bool(CNode *), CombinerAll> SendMessages;
    boost::signals2::signal<void(const CNode *)> InitializeNode;
    boost::signals2::signal<void(NodeId)> FinalizeNode;
};


CNodeSignals &GetNodeSignals();


enum
{
    LOCAL_NONE, // unknown
    LOCAL_IF, // address a local interface listens on
    LOCAL_BIND, // address explicit bound to
    LOCAL_UPNP, // address reported by UPnP
    LOCAL_MANUAL, // address explicitly specified (-externalip=)

    LOCAL_MAX
};

bool IsPeerAddrLocalGood(CNode *pnode);
void AdvertiseLocal(CNode *pnode);
void SetLimited(enum Network net, bool fLimited = true);
bool IsLimited(enum Network net);
bool IsLimited(const CNetAddr &addr);
bool AddLocal(const CService &addr, int nScore = LOCAL_NONE);
bool AddLocal(const CNetAddr &addr, int nScore = LOCAL_NONE);
bool RemoveLocal(const CService &addr);
bool SeenLocal(const CService &addr);
bool IsLocal(const CService &addr);
bool GetLocal(CService &addr, const CNetAddr *paddrPeer = nullptr);
bool IsReachable(enum Network net);
bool IsReachable(const CNetAddr &addr);
CAddress GetLocalAddress(const CNetAddr *paddrPeer = nullptr);


extern bool fDiscover;
extern bool fListen;
extern uint64_t nLocalServices;
extern uint64_t nLocalHostNonce;
extern CAddrMan addrman;

/** Maximum number of connections to simultaneously allow (aka connection slots) */
extern int nMaxConnections;

extern std::vector<CNode *> vNodes;
extern CCriticalSection cs_vNodes;
extern std::list<CNode *> vNodesDisconnected;
extern CCriticalSection cs_vNodesDisconnected;

extern std::map<CInv, CTransactionRef> mapRelay;
extern std::deque<std::pair<int64_t, CInv> > vRelayExpiration;
extern CCriticalSection cs_mapRelay;

extern std::vector<std::string> vAddedNodes;
extern CCriticalSection cs_vAddedNodes;

struct LocalServiceInfo
{
    int nScore;
    int nPort;
};

extern CCriticalSection cs_mapLocalHost;
extern std::map<CNetAddr, LocalServiceInfo> mapLocalHost;


class CNodeStats
{
public:
    NodeId nodeid;
    uint64_t nServices;
    bool fRelayTxes;
    int64_t nLastSend;
    int64_t nLastRecv;
    int64_t nTimeConnected; /** Calendar time this node was connected */
    uint64_t nStopwatchConnected; /** Stopwatch time this node was connected (in uSec) */
    int64_t nTimeOffset;
    std::string addrName;
    int nVersion;
    std::string cleanSubVer;
    bool fInbound;
    int nStartingHeight;
    uint64_t nSendBytes;
    uint64_t nRecvBytes;
    bool fWhitelisted;
    double dPingTime;
    double dPingWait;
    double dPingMin;
    //! What this peer sees as my address
    std::string addrLocal;
    //! Whether this peer supports CompactBlocks
    bool fSupportsCompactBlocks;
};


class CNetMessage
{
public:
    bool in_data; // parsing header (false) or data (true)

    CDataStream hdrbuf; // partially received header
    CMessageHeader hdr; // complete header
    unsigned int nHdrPos;

    CDataStream vRecv; // received message data
    unsigned int nDataPos;

    int64_t nTime; // calendar time (in microseconds) of message receipt.
    int64_t nStopwatch; // stopwatch time in microseconds of message receipt.  Used to calculate round trip latency.

    // default constructor builds an empty message object to accept assignment of real messages
    CNetMessage() : hdrbuf(0, 0), hdr({0, 0, 0, 0}), vRecv(0, 0)
    {
        in_data = false;
        nHdrPos = 0;
        nDataPos = 0;
        nTime = 0;
        nStopwatch = 0;
    }

    CNetMessage(const CMessageHeader::MessageStartChars &pchMessageStartIn, int nTypeIn, int nVersionIn)
        : hdrbuf(nTypeIn, nVersionIn), hdr(pchMessageStartIn), vRecv(nTypeIn, nVersionIn)
    {
        hdrbuf.resize(24);
        in_data = false;
        nHdrPos = 0;
        nDataPos = 0;
        nTime = 0;
        nStopwatch = 0;
    }

    // Returns true if this message has been completely received.  This is determined by checking the message size
    // field in the header against the number of payload bytes in this object.
    bool complete() const
    {
        if (!in_data)
            return false;
        return (hdr.nMessageSize == nDataPos);
    }

    // Returns the size of this message including header.  If the message is still being received
    // this call returns only what has been received.
    unsigned int size() const { return ((in_data) ? sizeof(CMessageHeader) : hdrbuf.size()) + nDataPos; }
    void SetVersion(int nVersionIn)
    {
        hdrbuf.SetVersion(nVersionIn);
        vRecv.SetVersion(nVersionIn);
    }

    int readHeader(const char *pch, unsigned int nBytes);
    int readData(const char *pch, unsigned int nBytes);
};


// BU cleaning up nodes as a global destructor creates many global destruction dependencies.  Instead use a function
// call.
#if 0
class CNetCleanup
{
public:
  CNetCleanup() {}
  ~CNetCleanup();
};
#endif

/** Information about a peer */
class CNode
{
#ifdef ENABLE_MUTRACE
    friend class CPrintSomePointers;
#endif

public:
    // All of the following variables should be atomics. They are potentially dynamic values because
    // of the XUPDATE message which can modify these values at any time.

    /** Does this node support mempool synchronization? */
    std::atomic<bool> canSyncMempoolWithPeers{false};
    /** Minimum supported mempool synchronization version */
    std::atomic<uint64_t> nMempoolSyncMinVersionSupported{0};
    /** Maximum supported mempool synchronization version */
    std::atomic<uint64_t> nMempoolSyncMaxVersionSupported{0};
    /** Tx concatenation supported (set by extversion) */
    std::atomic<bool> txConcat{false};
    /** set to true if this node support extversion */
    std::atomic<bool> extversionEnabled{false};
    /** set to true if the next expected message is extversion */
    std::atomic<bool> extversionExpected{false};
    /** set to true if this node is ok with no message checksum */
    std::atomic<bool> skipChecksum{false};
    /** Graphene min supported version */
    std::atomic<uint64_t> minGrapheneVersion{0};
    /** Graphene max supported version */
    std::atomic<uint64_t> maxGrapheneVersion{0};
    /** Negotiated graphene version.  Based on the other node and me, what's the best version to use? */
    std::atomic<uint64_t> negotiatedGrapheneVersion{GRAPHENE_NO_VERSION_SUPPORTED};
    // END section for atomic XVERSION variables


    // This is shared-locked whenever messages are processed.
    // Take it exclusive-locked to finish all ongoing processing
    CSharedCriticalSection csMsgSerializer;

    // socket
    SOCKET hSocket;

    /** Set to true if capd is enabled in this node (based on XVersion config) */
    bool isCapdEnabled = false;
    /** The "hook" into capd functionality */
    CapdNode *capd = nullptr;

    CCriticalSection cs_vSend;
    CDataStream ssSend GUARDED_BY(cs_vSend);
    size_t nSendOffset GUARDED_BY(cs_vSend); // offset inside the first vSendMsg already sent
    uint64_t nSendBytes GUARDED_BY(cs_vSend);
    std::deque<CSerializeData> vSendMsg GUARDED_BY(cs_vSend);
    std::deque<CSerializeData> vLowPrioritySendMsg GUARDED_BY(cs_vSend);
    std::atomic<uint64_t> nSendSize; // total size in bytes of all vSendMsg entries

    CCriticalSection csRecvGetData;
    std::deque<CInv> vRecvGetData GUARDED_BY(csRecvGetData);

    CCriticalSection cs_vRecvMsg;
    uint64_t nRecvBytes GUARDED_BY(cs_vRecvMsg);
    std::deque<CNetMessage> vRecvMsg GUARDED_BY(cs_vRecvMsg);
    std::deque<CNetMessage> vRecvMsg_handshake GUARDED_BY(cs_vRecvMsg);
    // the next message we receive from the socket
    CNetMessage msg GUARDED_BY(cs_vRecvMsg);
    CStatHistory<uint64_t> currentRecvMsgSize;

    uint64_t nServices;
    int nRecvVersion;

    /** Connection de-prioritization - Total useful bytes sent and received */
    std::atomic<uint64_t> nActivityBytes{0};
    /** The last time bytes were sent to the remote peer */
    std::atomic<int64_t> nLastSend{0};
    /** The last time bytes were received from the remote peer */
    std::atomic<int64_t> nLastRecv{0};
    /** Calendar time this node was connected */
    std::atomic<int64_t> nTimeConnected;
    /** Stopwatch time this node was connected */
    std::atomic<uint64_t> nStopwatchConnected;

    int64_t nTimeOffset;

    /** The address of the remote peer */
    CAddress addr;

    /** The address the remote peer advertised in its version message */
    CAddress addrFrom_advertised;

    std::string addrName;
    const char *currentCommand; // if in the middle of the send, this is the command type
    /** The the remote peer sees us as this address (may be different than our IP due to NAT) */
    CService addrLocal;
    int nVersion;

    /** used to make processing serial when version handshake is taking place */
    CCriticalSection csSerialPhase;

    /** the intial extversion message sent in the handshake */
    CCriticalSection cs_extversion;
    CExtversionMessage extversion GUARDED_BY(cs_extversion);

    /** strSubVer is whatever byte array we read from the wire. However, this field is intended
        to be printed out, displayed to humans in various forms and so on. So we sanitize it and
        store the sanitized version in cleanSubVer. The original should be used when dealing with
        the network or wire types and the cleaned string used when displayed or logged.
    */
    std::string strSubVer, cleanSubVer;

    /** This peer can bypass DoS banning. */
    bool fWhitelisted;
    /** If true this node is being used as a short lived feeler. */
    bool fFeeler;
    bool fOneShot;
    bool fClient;

    /** after BIP159 */
    bool m_limited_node;

    /** If true a remote node initiated the connection.  If false, we initiated.
        The protocol is slightly asymmetric:
        initial version exchange
        stop ADDR flooding in preparation for a network-wide eclipse attack
        stop connection slot attack via eviction of stale (connected but no data) inbound connections
        stop fingerprinting by seeding fake addresses and checking for them later by ignoring outbound getaddr
    */
    bool fInbound;
    bool fAutoOutbound; // any outbound node not connected with -addnode, connect-thinblock or -connect
    bool fNetworkNode; // any outbound node
    int64_t tVersionSent;

    std::atomic<bool> fDisconnect;
    std::atomic<bool> fDisconnectRequest;
    // We use fRelayTxes for two purposes -
    // a) it allows us to not relay tx invs before receiving the peer's version message
    // b) the peer may tell us in its version message that we should not relay tx invs
    //    unless it loads a bloom filter.
    bool fRelayTxes;
    bool fSentAddr;
    CSemaphoreGrant grantOutbound;

    CCriticalSection cs_filter;
    /** A bloom filter which is used by SPV wallets */
    CBloomFilter *pfilter GUARDED_BY(cs_filter);
    /** Xtreme Thinblocks bloom filter which is send with a get_xthin request */
    CBloomFilter *pThinBlockFilter GUARDED_BY(cs_filter);

    std::atomic<int> nRefCount;
    NodeId id;

    //! Accumulated misbehavior score for this peer.
    std::atomic<double> nMisbehavior{0};
    std::atomic<int64_t> nLastMisbehaviorTime{0};

    //! Whether this peer should be disconnected and banned (unless whitelisted).
    std::atomic<bool> fShouldBan{false};
    std::atomic<int> nBanType{-1};

    // General thintype critical section to ensure that no
    // two thintype blocks from the "same" peer can be processed at
    // the same time.
    CCriticalSection cs_thintype;

    // Xtreme Thinblocks
    /** Max xthin bloom filter size (in bytes) that our peer will accept */
    std::atomic<uint32_t> nXthinBloomfilterSize;

    // Graphene blocks
    /** Stores the grapheneblock salt to be used for this peer */
    std::atomic<uint64_t> gr_shorttxidk0;
    std::atomic<uint64_t> gr_shorttxidk1;

    // Compact Blocks
    /** Stores the compactblock salt to be used for this peer */
    std::atomic<uint64_t> shorttxidk0;
    std::atomic<uint64_t> shorttxidk1;
    /** Does this peer support CompactBlocks */
    std::atomic<bool> fSupportsCompactBlocks;

    CCriticalSection cs_nAvgBlkResponseTime;
    double nAvgBlkResponseTime GUARDED_BY(cs_nAvgBlkResponseTime);
    std::atomic<uint64_t> nMaxBlocksInTransit;

    unsigned short addrFromPort;

    std::atomic<bool> fSuccessfullyConnected;

protected:
    // Basic fuzz-testing
    void Fuzz(int nChance); // modifies ssSend

public:
#ifdef DEBUG
    friend UniValue getstructuresizes(const UniValue &params, bool fHelp);
#endif
    uint256 hashContinue;
    int nStartingHeight;

    // flood relay
    std::vector<CAddress> vAddrToSend GUARDED_BY(cs_vSend);
    CRollingBloomFilter addrKnown;
    std::set<uint256> setKnown;
    int64_t nNextAddrSend;
    int64_t nNextLocalAddrSend;

    // inventory based relay
    CRollingFastFilter<4 * 1024 * 1024> filterInventoryKnown;
    CCriticalSection cs_inventory;
    std::vector<CInv> vInventoryToSend GUARDED_BY(cs_inventory);
    int64_t nNextInvSend;
    // Used for headers announcements - unfiltered blocks to relay
    // Also protected by cs_inventory
    std::vector<uint256> vBlockHashesToAnnounce;

    // Ping time measurement:
    // The pong reply we're expecting, or 0 if no pong expected.
    uint64_t nPingNonceSent;
    // Time (in usec) the last ping was sent, or 0 if no ping was ever sent.
    int64_t nPingUsecStart;
    // Last measured round-trip time.
    int64_t nPingUsecTime;
    // Best measured round-trip time.
    int64_t nMinPingUsecTime;
    // Whether a ping is requested.
    bool fPingQueued;
    // Whether an ADDR was requested.
    std::atomic<bool> fGetAddr;

    // BU instrumentation
    // track the number of bytes sent to this node
    CStatHistory<unsigned int> bytesSent;
    // track the number of bytes received from this node
    CStatHistory<unsigned int> bytesReceived;
    // track the average round trip latency for transaction requests to this node
    CStatHistory<unsigned int> txReqLatency;
    // track the # of times this node is the first to send us a transaction INV
    CStatHistory<unsigned int> firstTx;
    // track the # of times this node is the first to send us a block INV
    CStatHistory<unsigned int> firstBlock;
    // track the # of times we sent this node a block
    CStatHistory<unsigned int> blocksSent;
    // track the # of times we sent this node a transaction
    CStatHistory<unsigned int> txsSent;
    // track the # of times we sent this node a transaction
    CStatHistory<unsigned int> sendGap;
    // track the # of times we sent this node a transaction
    CStatHistory<unsigned int> recvGap;


    CNode(SOCKET hSocketIn, const CAddress &addrIn, const std::string &addrNameIn = "", bool fInboundIn = false);
    ~CNode();

private:
    // Network usage totals
    static std::atomic<uint64_t> nTotalBytesRecv;
    static std::atomic<uint64_t> nTotalBytesSent;

    // outbound limit & stats
    static std::atomic<uint64_t> nMaxOutboundTotalBytesSentInCycle;
    static std::atomic<uint64_t> nMaxOutboundCycleStartTime;
    static std::atomic<uint64_t> nMaxOutboundLimit;
    static std::atomic<uint64_t> nMaxOutboundTimeframe;

    CNode(const CNode &);
    void operator=(const CNode &);

public:
    NodeId GetId() const { return id; }
    int GetRefCount()
    {
        DbgAssert(nRefCount >= 0, );
        return nRefCount;
    }

    /** Updates node configuration variables based on extversion data in the extversion member variable */
    void ReadConfigFromExtversion();

    // requires LOCK(cs_vRecvMsg)
    unsigned int GetTotalRecvSize() EXCLUSIVE_LOCKS_REQUIRED(cs_vRecvMsg)
    {
        AssertLockHeld(cs_vRecvMsg);
        unsigned int total = 0;
        for (const CNetMessage &message : vRecvMsg)
            total += message.vRecv.size() + 24;
        return total;
    }

    unsigned int GetSendMsgSize()
    {
        LOCK(cs_vSend);
        return vSendMsg.size();
    }


    // requires LOCK(cs_vRecvMsg)
    bool ReceiveMsgBytes(const char *pch, unsigned int nBytes) EXCLUSIVE_LOCKS_REQUIRED(cs_vRecvMsg);

    // Examine the current message (msg) to see if block or thintype blocks have begun downloading data.
    std::atomic<bool> fDownloading{false};
    void LookAhead() EXCLUSIVE_LOCKS_REQUIRED(cs_vRecvMsg);

    void SetRecvVersion(int nVersionIn)
    {
        LOCK(cs_vRecvMsg);
        nRecvVersion = nVersionIn;
        for (CNetMessage &message : vRecvMsg)
            message.SetVersion(nVersionIn);
    }

    const CMessageHeader::MessageStartChars &GetMagic(const CChainParams &params) const
    {
        if (netMagic.Value() != 0)
        {
            netOverride[0] = netMagic.Value() & 255;
            netOverride[1] = (netMagic.Value() >> 8) & 255;
            netOverride[2] = (netMagic.Value() >> 16) & 255;
            netOverride[3] = (netMagic.Value() >> 24) & 255;
            return netOverride;
        }
        return params.MessageStart();
    }

    CNode *AddRef()
    {
        nRefCount++;
        return this;
    }

    void Release()
    {
        DbgAssert(nRefCount > 0, );
        nRefCount--;
    }

    // BUIP010:
    bool ThinBlockCapable()
    {
        if (nServices & NODE_XTHIN)
            return true;
        return false;
    }

    // BUIPXXX:
    bool GrapheneCapable()
    {
        if (nServices & NODE_GRAPHENE)
            return true;
        return false;
    }

    bool CompactBlockCapable()
    {
        if (fSupportsCompactBlocks)
            return true;
        return false;
    }

    void AddAddressKnown(const CAddress &_addr) { addrKnown.insert(_addr.GetKey()); }
    void PushAddress(const CAddress &_addr, FastRandomContext &insecure_rand)
    {
        LOCK(cs_vSend);
        // Known checking here is only to save space from duplicates.
        // SendMessages will filter it again for knowns that were added
        // after addresses were pushed.
        if (_addr.IsValid() && !addrKnown.contains(_addr.GetKey()))
        {
            if (vAddrToSend.size() >= MAX_ADDR_TO_SEND)
            {
                vAddrToSend[insecure_rand.randrange(vAddrToSend.size())] = _addr;
            }
            else
            {
                vAddrToSend.push_back(_addr);
            }
        }
    }


    void AddInventoryKnown(const CInv &inv)
    {
        LOCK(cs_inventory);
        filterInventoryKnown.insert(inv.hash);
    }

    /**
     * Add a reference of a new INV message to the inventory
     *
     * @param[in] inv reference to new INV object
     * @param[in] force whether or not force the push in case the INV was already added
     */
    void PushInventory(const CInv &inv, bool force = false)
    {
        LOCK(cs_inventory);
        if (!force && inv.type == MSG_TX && filterInventoryKnown.contains(inv.hash))
            return;
        vInventoryToSend.push_back(inv);
    }

    /** Get size of INVs to be sent in a thread safe way*/
    unsigned int GetInventoryToSendSize()
    {
        LOCK(cs_inventory);
        return vInventoryToSend.size();
    }

    /**
     * Add a reference of a new hash block to the list of blocks need to be announced
     *
     * @param[in] hash reference to the hash of the new block to announce
     */
    void PushBlockHash(const uint256 &hash)
    {
        LOCK(cs_inventory);
        vBlockHashesToAnnounce.push_back(hash);
    }

    // TODO: Document the postcondition of this function.  Is cs_vSend locked?
    void BeginMessage(const char *pszCommand) EXCLUSIVE_LOCK_FUNCTION(cs_vSend);

    // TODO: Document the precondition of this function.  Is cs_vSend locked?
    void AbortMessage() UNLOCK_FUNCTION(cs_vSend);

    // TODO: Document the precondition of this function.  Is cs_vSend locked?
    void EndMessage() UNLOCK_FUNCTION(cs_vSend);

    void PushVersion();


    void PushMessage(const char *pszCommand)
    {
        try
        {
            BeginMessage(pszCommand);
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template <typename T1>
    void PushMessage(const char *pszCommand, const T1 &a1)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template <typename T1, typename T2>
    void PushMessage(const char *pszCommand, const T1 &a1, const T2 &a2)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template <typename T1, typename T2, typename T3>
    void PushMessage(const char *pszCommand, const T1 &a1, const T2 &a2, const T3 &a3)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template <typename T1, typename T2, typename T3, typename T4>
    void PushMessage(const char *pszCommand, const T1 &a1, const T2 &a2, const T3 &a3, const T4 &a4)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template <typename T1, typename T2, typename T3, typename T4, typename T5>
    void PushMessage(const char *pszCommand, const T1 &a1, const T2 &a2, const T3 &a3, const T4 &a4, const T5 &a5)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template <typename T1, typename T2, typename T3, typename T4, typename T5, typename T6>
    void PushMessage(const char *pszCommand,
        const T1 &a1,
        const T2 &a2,
        const T3 &a3,
        const T4 &a4,
        const T5 &a5,
        const T6 &a6)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5 << a6;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template <typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7>
    void PushMessage(const char *pszCommand,
        const T1 &a1,
        const T2 &a2,
        const T3 &a3,
        const T4 &a4,
        const T5 &a5,
        const T6 &a6,
        const T7 &a7)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5 << a6 << a7;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template <typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7, typename T8>
    void PushMessage(const char *pszCommand,
        const T1 &a1,
        const T2 &a2,
        const T3 &a3,
        const T4 &a4,
        const T5 &a5,
        const T6 &a6,
        const T7 &a7,
        const T8 &a8)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template <typename T1,
        typename T2,
        typename T3,
        typename T4,
        typename T5,
        typename T6,
        typename T7,
        typename T8,
        typename T9>
    void PushMessage(const char *pszCommand,
        const T1 &a1,
        const T2 &a2,
        const T3 &a3,
        const T4 &a4,
        const T5 &a5,
        const T6 &a6,
        const T7 &a7,
        const T8 &a8,
        const T9 &a9)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8 << a9;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    /**
     * Check if it is flagged for banning, and if so ban it and disconnect.
     */
    void DisconnectIfBanned();

    void CloseSocketDisconnect();

    //! returns the name of this node for logging.  Respects the user's choice to not log the node's IP
    std::string GetLogName()
    {
        std::string idstr = std::to_string(id);
        if (fLogIPs)
            return addrName + " (" + idstr + ")";
        return idstr;
    }

    //! Disconnects after receiving all the blocks we are waiting for.  Typically this happens if the node is
    // responding slowly compared to other nodes.
    void InitiateGracefulDisconnect()
    {
        if (!fDisconnectRequest)
        {
            fDisconnectRequest = true;
            // But we need to make sure this connection is actually alive by attempting a send
            // otherwise there is no reason to wait (up to PING_INTERVAL seconds).
            // If the other side of a TCP connection goes silent you need to do a send to find out that its dead.
            fPingQueued = true;
        }
    }

    void copyStats(CNodeStats &stats);

    bool IsCapdEnabled() { return isCapdEnabled; }
    // Network stats
    static void RecordBytesRecv(uint64_t bytes);
    static void RecordBytesSent(uint64_t bytes);

    static uint64_t GetTotalBytesRecv();
    static uint64_t GetTotalBytesSent();

    //! set the max outbound target in bytes
    static void SetMaxOutboundTarget(uint64_t limit);
    static uint64_t GetMaxOutboundTarget();

    //! set the timeframe for the max outbound target
    static void SetMaxOutboundTimeframe(uint64_t timeframe);
    static uint64_t GetMaxOutboundTimeframe();

    //! check if the outbound target is reached
    // if param historicalBlockServingLimit is set true, the function will
    // response true if the limit for serving historical blocks has been reached
    static bool OutboundTargetReached(bool historicalBlockServingLimit);

    //! response the bytes left in the current max outbound cycle
    // in case of no limit, it will always response 0
    static uint64_t GetOutboundTargetBytesLeft();

    //! response the time in second left in the current max outbound cycle
    // in case of no limit, it will always response 0
    static uint64_t GetMaxOutboundTimeLeftInCycle();
};

// Exception-safe class for holding a reference to a CNode
class CNodeRef
{
    void AddRef()
    {
        if (_pnode)
            _pnode->AddRef();
    }

    void Release()
    {
        if (_pnode)
        {
            // Make the noderef null before releasing, to ensure a user can't get freed memory from us
            CNode *tmp = _pnode;
            _pnode = nullptr;
            tmp->Release();
        }
    }

public:
    CNodeRef(CNode *pnode = nullptr) : _pnode(pnode) { AddRef(); }
    CNodeRef(const CNodeRef &other) : _pnode(other._pnode) { AddRef(); }
    ~CNodeRef() { Release(); }
    CNode &operator*() const { return *_pnode; };
    CNode *operator->() const { return _pnode; };
    // Returns true if this reference is not null
    explicit operator bool() const { return _pnode; }
    // Access the raw pointer
    CNode *get() const { return _pnode; }
    // Assignment -- destroys any reference to the current node and adds a ref to the new one
    CNodeRef &operator=(CNode *pnode)
    {
        if (pnode != _pnode)
        {
            Release();
            _pnode = pnode;
            AddRef();
        }
        return *this;
    }
    // Assignment -- destroys any reference to the current node and adds a ref to the new one
    CNodeRef &operator=(const CNodeRef &other) { return operator=(other._pnode); }

private:
    CNode *_pnode;
};

typedef std::vector<CNodeRef> VNodeRefs;

class CTransaction;
void RelayTransaction(const CTransactionRef ptx);

/** Access to the (IP) address database (peers.dat) */
class CAddrDB
{
private:
    fs::path pathAddr;

public:
    CAddrDB();
    bool Write(const CAddrMan &addr);
    bool Read(CAddrMan &addr);
    bool Read(CAddrMan &addr, CDataStream &ssPeers);
};

/** Return a timestamp in the future (in microseconds) for exponentially distributed events. */
int64_t PoissonNextSend(int64_t nNow, int average_interval_seconds);

#endif // NEXA_NET_H
