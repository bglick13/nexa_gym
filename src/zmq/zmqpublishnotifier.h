// Copyright (c) 2015 The Bitcoin Core developers
// Copyright (c) 2015-2020 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef NEXA_ZMQ_ZMQPUBLISHNOTIFIER_H
#define NEXA_ZMQ_ZMQPUBLISHNOTIFIER_H

#include "zmqabstractnotifier.h"

class CBlockIndex;

class CZMQAbstractPublishNotifier : public CZMQAbstractNotifier
{
public:
    bool Initialize(void *pcontext);
    void Shutdown();
};

class CZMQPublishHashBlockNotifier : public CZMQAbstractPublishNotifier
{
public:
    bool NotifyBlock(const CBlockIndex *pindex);
};

class CZMQPublishHashTransactionNotifier : public CZMQAbstractPublishNotifier
{
public:
    bool NotifyTransaction(const CTransactionRef &ptx);
};

class CZMQPublishHashDoubleSpendNotifier : public CZMQAbstractPublishNotifier
{
public:
    bool NotifyDoubleSpend(const CTransactionRef ptx);
};

class CZMQPublishRawBlockNotifier : public CZMQAbstractPublishNotifier
{
public:
    bool NotifyBlock(const CBlockIndex *pindex);
};

class CZMQPublishRawTransactionNotifier : public CZMQAbstractPublishNotifier
{
public:
    bool NotifyTransaction(const CTransactionRef &ptx);
};

class CZMQPublishRawDoubleSpendNotifier : public CZMQAbstractPublishNotifier
{
public:
    bool NotifyDoubleSpend(const CTransactionRef ptx);
};

#endif // NEXA_ZMQ_ZMQPUBLISHNOTIFIER_H
