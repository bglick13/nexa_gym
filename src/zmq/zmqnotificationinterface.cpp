// Copyright (c) 2015 The Bitcoin Core developers
// Copyright (c) 2015-2020 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zmqnotificationinterface.h"
#include "zmqpublishnotifier.h"

#include "main.h"
#include "streams.h"
#include "util.h"
#include "version.h"

void zmqError(const char *str) { LOG(ZMQ, "zmq: Error: %s, errno=%s\n", str, zmq_strerror(errno)); }
CZMQNotificationInterface::CZMQNotificationInterface() : pcontext(nullptr) {}
CZMQNotificationInterface::~CZMQNotificationInterface()
{
    Shutdown();

    for (std::list<CZMQAbstractNotifier *>::iterator i = notifiers.begin(); i != notifiers.end(); ++i)
    {
        delete *i;
    }
}

std::list<const CZMQAbstractNotifier *> CZMQNotificationInterface::GetActiveNotifiers() const
{
    std::list<const CZMQAbstractNotifier *> result;
    for (const auto *n : notifiers)
    {
        result.push_back(n);
    }
    return result;
}

CZMQNotificationInterface *CZMQNotificationInterface::CreateWithArguments(
    const std::map<std::string, std::string> &args)
{
    CZMQNotificationInterface *notificationInterface = nullptr;
    std::map<std::string, CZMQNotifierFactory> factories;
    std::list<CZMQAbstractNotifier *> notifiers;

    factories["pubhashblock"] = CZMQAbstractNotifier::Create<CZMQPublishHashBlockNotifier>;
    factories["pubhashtx"] = CZMQAbstractNotifier::Create<CZMQPublishHashTransactionNotifier>;
    factories["pubhashds"] = CZMQAbstractNotifier::Create<CZMQPublishHashDoubleSpendNotifier>;
    factories["pubrawblock"] = CZMQAbstractNotifier::Create<CZMQPublishRawBlockNotifier>;
    factories["pubrawtx"] = CZMQAbstractNotifier::Create<CZMQPublishRawTransactionNotifier>;
    factories["pubrawds"] = CZMQAbstractNotifier::Create<CZMQPublishRawDoubleSpendNotifier>;

    for (std::map<std::string, CZMQNotifierFactory>::const_iterator i = factories.begin(); i != factories.end(); ++i)
    {
        std::map<std::string, std::string>::const_iterator j = args.find("-zmq" + i->first);
        if (j != args.end())
        {
            CZMQNotifierFactory factory = i->second;
            std::string address = j->second;
            CZMQAbstractNotifier *notifier = factory();
            notifier->SetType(i->first);
            notifier->SetAddress(address);
            notifiers.push_back(notifier);
        }
    }

    if (!notifiers.empty())
    {
        notificationInterface = new CZMQNotificationInterface();
        notificationInterface->notifiers = notifiers;

        if (!notificationInterface->Initialize())
        {
            delete notificationInterface;
            notificationInterface = nullptr;
        }
    }

    return notificationInterface;
}

// Called at startup to conditionally set up ZMQ socket(s)
bool CZMQNotificationInterface::Initialize()
{
    LOG(ZMQ, "zmq: Initialize notification interface\n");
    assert(!pcontext);

    pcontext = zmq_init(1);

    if (!pcontext)
    {
        zmqError("Unable to initialize context");
        return false;
    }

    std::list<CZMQAbstractNotifier *>::iterator i = notifiers.begin();
    for (; i != notifiers.end(); ++i)
    {
        CZMQAbstractNotifier *notifier = *i;
        if (notifier->Initialize(pcontext))
        {
            LOG(ZMQ, "  Notifier %s ready (address = %s)\n", notifier->GetType(), notifier->GetAddress());
        }
        else
        {
            LOG(ZMQ, "  Notifier %s failed (address = %s)\n", notifier->GetType(), notifier->GetAddress());
            break;
        }
    }

    if (i != notifiers.end())
    {
        Shutdown();
        return false;
    }

    return true;
}

// Called during shutdown sequence
void CZMQNotificationInterface::Shutdown()
{
    LOG(ZMQ, "zmq: Shutdown notification interface\n");
    if (pcontext)
    {
        for (std::list<CZMQAbstractNotifier *>::iterator i = notifiers.begin(); i != notifiers.end(); ++i)
        {
            CZMQAbstractNotifier *notifier = *i;
            LOG(ZMQ, "   Shutdown notifier %s at %s\n", notifier->GetType(), notifier->GetAddress());
            notifier->Shutdown();
        }
        zmq_ctx_destroy(pcontext);

        pcontext = 0;
    }
}

void CZMQNotificationInterface::UpdatedBlockTip(const CBlockIndex *pindex)
{
    for (std::list<CZMQAbstractNotifier *>::iterator i = notifiers.begin(); i != notifiers.end();)
    {
        CZMQAbstractNotifier *notifier = *i;
        if (notifier->NotifyBlock(pindex))
        {
            i++;
        }
        else
        {
            notifier->Shutdown();
            i = notifiers.erase(i);
        }
    }
}

void CZMQNotificationInterface::SyncTransaction(const CTransactionRef &ptx, const ConstCBlockRef pblock, int txIndex)
{
    for (std::list<CZMQAbstractNotifier *>::iterator i = notifiers.begin(); i != notifiers.end();)
    {
        CZMQAbstractNotifier *notifier = *i;
        if (notifier->NotifyTransaction(ptx))
        {
            i++;
        }
        else
        {
            notifier->Shutdown();
            i = notifiers.erase(i);
        }
    }
}

void CZMQNotificationInterface::SyncDoubleSpend(const CTransactionRef ptx)
{
    for (std::list<CZMQAbstractNotifier *>::iterator i = notifiers.begin(); i != notifiers.end();)
    {
        CZMQAbstractNotifier *notifier = *i;
        if (notifier->NotifyDoubleSpend(ptx))
        {
            i++;
        }
        else
        {
            notifier->Shutdown();
            i = notifiers.erase(i);
        }
    }
}


CZMQNotificationInterface *pzmqNotificationInterface = nullptr;
