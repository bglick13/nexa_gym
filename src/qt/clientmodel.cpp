// Copyright (c) 2011-2015 The Bitcoin Core developers
// Copyright (c) 2015-2020 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "clientmodel.h"

#include "bantablemodel.h"
#include "guiconstants.h"
#include "peertablemodel.h"

#include "capd/capd.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "clientversion.h"
#include "dosman.h"
#include "net.h"
#include "txmempool.h"
#include "txorphanpool.h"
#include "ui_interface.h"
#include "util.h"

#include <stdint.h>

#include <QDebug>
#include <QTimer>

class CBlockIndex;

static const int64_t nClientStartupTime = GetTime();
static int64_t nLastBlockTipUpdateNotification = 0;

ClientModel::ClientModel(OptionsModel *_optionsModel, UnlimitedModel *ul, QObject *parent)
    : QObject(parent), unlimitedModel(ul), lastBlockTime(0), optionsModel(_optionsModel), peerTableModel(0),
      banTableModel(0), pollTimer1(0), pollTimer2(0), pollTimer3(0)
{
    peerTableModel = new PeerTableModel(this);
    banTableModel = new BanTableModel(this);

    pollTimer1 = new QTimer(this);
    connect(pollTimer1, SIGNAL(timeout()), this, SLOT(updateTimer1()));
    pollTimer1->start(MODEL_UPDATE_DELAY1);

    pollTimer2 = new QTimer(this);
    connect(pollTimer2, SIGNAL(timeout()), this, SLOT(updateTimer2()));
    pollTimer2->start(MODEL_UPDATE_DELAY2);

    pollTimer3 = new QTimer(this);
    connect(pollTimer3, SIGNAL(timeout()), this, SLOT(updateTimerTransactionRate()));
    pollTimer3->start(TX_RATE_RESOLUTION_MILLIS);

    subscribeToCoreSignals();
}

ClientModel::~ClientModel() { unsubscribeFromCoreSignals(); }
int ClientModel::getNumConnections(unsigned int flags) const
{
    LOCK(cs_vNodes);
    if (flags == CONNECTIONS_ALL) // Shortcut if we want total
        return vNodes.size();

    int nNum = 0;
    for (const CNode *pnode : vNodes)
        if (flags & (pnode->fInbound ? CONNECTIONS_IN : CONNECTIONS_OUT))
            nNum++;

    return nNum;
}

int ClientModel::getNumBlocks() const { return chainActive.Height(); }
quint64 ClientModel::getTotalBytesRecv() const { return CNode::GetTotalBytesRecv(); }
quint64 ClientModel::getTotalBytesSent() const { return CNode::GetTotalBytesSent(); }
int ClientModel::getHeaderTipHeight() const
{
    if (!pindexBestHeader)
        return 0;
    return pindexBestHeader.load()->height();
}
int64_t ClientModel::getHeaderTipTime() const
{
    if (!pindexBestHeader)
        return 0;
    return pindexBestHeader.load()->GetBlockTime();
}
QDateTime ClientModel::getLastBlockDate() const
{
    if (chainActive.Tip())
        lastBlockTime = chainActive.Tip()->GetBlockTime();
    else
        lastBlockTime = Params().GenesisBlock().GetBlockTime(); // Genesis block's time of current network

    return QDateTime::fromTime_t(lastBlockTime);
}

long ClientModel::getMempoolSize() const { return mempool.size(); }
long ClientModel::getOrphanPoolSize() const { return orphanpool.GetOrphanPoolSize(); }
long ClientModel::getCapdMessagePoolSize() const { return msgpool.Count(); }
size_t ClientModel::getMempoolDynamicUsage() const { return mempool.DynamicMemoryUsage(); }
double ClientModel::getVerificationProgress(const CBlockIndex *tipIn) const
{
    CBlockIndex *tip = const_cast<CBlockIndex *>(tipIn);
    if (!tip)
    {
        tip = chainActive.Tip();
    }
    return Checkpoints::GuessVerificationProgress(Params().Checkpoints(), tip, !fCheckpointsEnabled);
}

void ClientModel::updateTimer1()
{
    // no locking required at this point
    // the following calls will acquire the required lock
    Q_EMIT mempoolSizeChanged(getMempoolSize(), getMempoolDynamicUsage());

    // only request updates to time since last block if we aren't in initial sync
    if (IsChainNearlySyncd())
        Q_EMIT timeSinceLastBlockChanged(lastBlockTime);
}

void ClientModel::updateTimer2()
{
    // no locking required at this point
    // the following calls will aquire the required lock
    Q_EMIT orphanPoolSizeChanged(getOrphanPoolSize());

    Q_EMIT messagePoolSizeChanged(getCapdMessagePoolSize());

    Q_EMIT bytesChanged(getTotalBytesRecv(), getTotalBytesSent());

    thindata.FillThinBlockQuickStats(thinStats);
    Q_EMIT thinBlockPropagationStatsChanged(thinStats);

    compactdata.FillCompactBlockQuickStats(compactStats);
    Q_EMIT compactBlockPropagationStatsChanged(compactStats);

    graphenedata.FillGrapheneQuickStats(grapheneStats);
    Q_EMIT grapheneBlockPropagationStatsChanged(grapheneStats);

    uiInterface.BannedListChanged();
}

void ClientModel::updateTimerTransactionRate()
{
    double smoothedTps = 0.0, instantaneousTps = 0.0, peakTps = 0.0;
    mempool.GetTransactionRateStatistics(smoothedTps, instantaneousTps, peakTps);

    // This is a bit of a hack, but don't emit this signal until peakTps is > 0
    // This will prevent us from registering empty samples in the transaction graph
    // as there is presently a 10 second window where the statistics aren't updated
    // to avoid a large spike when mempool.dat is loaded from disk during start-up
    if (peakTps > 0.0)
        Q_EMIT transactionsPerSecondChanged(smoothedTps, instantaneousTps, peakTps);
}


void ClientModel::updateNumConnections(int numConnections) { Q_EMIT numConnectionsChanged(numConnections); }
void ClientModel::updateAlert() { Q_EMIT alertsChanged(getStatusBarWarnings()); }
bool ClientModel::inInitialBlockDownload() const { return IsInitialBlockDownload(); }
enum BlockSource ClientModel::getBlockSource() const
{
    if (fReindex)
        return BLOCK_SOURCE_REINDEX;
    else if (fImporting)
        return BLOCK_SOURCE_DISK;
    else if (getNumConnections() > 0)
        return BLOCK_SOURCE_NETWORK;

    return BLOCK_SOURCE_NONE;
}

QString ClientModel::getStatusBarWarnings() const { return QString::fromStdString(GetWarnings("gui")); }
OptionsModel *ClientModel::getOptionsModel() { return optionsModel; }
PeerTableModel *ClientModel::getPeerTableModel() { return peerTableModel; }
BanTableModel *ClientModel::getBanTableModel() { return banTableModel; }
QString ClientModel::formatFullVersion() const { return QString::fromStdString(FormatFullVersion()); }
QString ClientModel::formatSubVersion() const
{
    return QString::fromStdString(FormatSubVersion(CLIENT_NAME, CLIENT_VERSION, BUComments));
}
bool ClientModel::isReleaseVersion() const { return CLIENT_VERSION_IS_RELEASE; }
QString ClientModel::clientName() const { return QString::fromStdString(CLIENT_NAME); }
QString ClientModel::formatClientStartupTime() const
{
    QString time_format = "MMM d yyyy, HH:mm:ss";
    // return QDateTime::fromTime_t(GetStartupTime()).toString(time_format);
    return QDateTime::fromTime_t(GetStartupTime()).toString();
}

QString ClientModel::dataDir() const { return QString::fromStdString(GetDataDir().string()); }
void ClientModel::updateBanlist() { banTableModel->refresh(); }
// Handlers for core signals
static void ShowProgress(ClientModel *clientmodel, const std::string &title, int nProgress)
{
    // emits signal "showProgress"
    QMetaObject::invokeMethod(clientmodel, "showProgress", Qt::QueuedConnection,
        Q_ARG(QString, QString::fromStdString(title)), Q_ARG(int, nProgress));
}

static void NotifyNumConnectionsChanged(ClientModel *clientmodel, int newNumConnections)
{
    // Too noisy: qDebug() << "NotifyNumConnectionsChanged: " + QString::number(newNumConnections);
    QMetaObject::invokeMethod(clientmodel, "updateNumConnections", Qt::QueuedConnection, Q_ARG(int, newNumConnections));
}

static void NotifyAlertChanged(ClientModel *clientmodel)
{
    qDebug() << "NotifyAlertChanged";
    QMetaObject::invokeMethod(clientmodel, "updateAlert", Qt::QueuedConnection);
}

static void BannedListChanged(ClientModel *clientmodel)
{
    qDebug() << QString("%1: Requesting update for peer banlist").arg(__func__);
    QMetaObject::invokeMethod(clientmodel, "updateBanlist", Qt::QueuedConnection);
}

static void BlockTipChanged(ClientModel *clientmodel, bool initialSync, const CBlockIndex *pIndex, bool fHeader)
{
    // lock free async UI updates in case we have a new block tip
    // during initial sync, only update the UI if the last update
    // was > 250ms (MODEL_UPDATE_DELAY1) ago
    int64_t now = 0;
    if (initialSync)
        now = GetTimeMillis();

    clientmodel->lastBlockTime = pIndex->GetBlockTime();
    // if we are in-sync, update the UI regardless of last update time
    if (!initialSync || now - nLastBlockTipUpdateNotification > MODEL_UPDATE_DELAY1)
    {
        // pass a async signal to the UI thread
        QMetaObject::invokeMethod(clientmodel, "numBlocksChanged", Qt::QueuedConnection, Q_ARG(int, pIndex->height()),
            Q_ARG(QDateTime, QDateTime::fromTime_t(clientmodel->lastBlockTime)),
            Q_ARG(double, clientmodel->getVerificationProgress(pIndex)), Q_ARG(bool, fHeader));
        nLastBlockTipUpdateNotification = now;
    }
}

void ClientModel::subscribeToCoreSignals()
{
    // Connect signals to client
    uiInterface.ShowProgress.connect(boost::bind(ShowProgress, this, boost::arg<1>(), boost::arg<2>()));
    uiInterface.NotifyNumConnectionsChanged.connect(boost::bind(NotifyNumConnectionsChanged, this, boost::arg<1>()));
    uiInterface.NotifyAlertChanged.connect(boost::bind(NotifyAlertChanged, this));
    uiInterface.BannedListChanged.connect(boost::bind(BannedListChanged, this));
    uiInterface.NotifyBlockTip.connect(
        boost::bind(BlockTipChanged, this, boost::arg<1>(), boost::arg<2>(), boost::arg<3>()));
    uiInterface.NotifyHeaderTip.connect(
        boost::bind(BlockTipChanged, this, boost::arg<1>(), boost::arg<2>(), boost::arg<3>()));
}

void ClientModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from client
    uiInterface.ShowProgress.disconnect(boost::bind(ShowProgress, this, boost::arg<1>(), boost::arg<2>()));
    uiInterface.NotifyNumConnectionsChanged.disconnect(boost::bind(NotifyNumConnectionsChanged, this, boost::arg<1>()));
    uiInterface.NotifyAlertChanged.disconnect(boost::bind(NotifyAlertChanged, this));
    uiInterface.BannedListChanged.disconnect(boost::bind(BannedListChanged, this));
    uiInterface.NotifyBlockTip.disconnect(
        boost::bind(BlockTipChanged, this, boost::arg<1>(), boost::arg<2>(), boost::arg<3>()));
    uiInterface.NotifyHeaderTip.disconnect(
        boost::bind(BlockTipChanged, this, boost::arg<1>(), boost::arg<2>(), boost::arg<3>()));
}
