// Copyright (c) 2017 Amaury SÉCHET
// Copyright (c) 2017-2019 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef NEXA_CONFIG2_H
#define NEXA_CONFIG2_H

#include <boost/noncopyable.hpp>

#include <cstdint>

class CChainParams;

class Config : public boost::noncopyable
{
public:
    virtual const CChainParams &GetChainParams() const = 0;
    virtual void SetCashAddrEncoding(bool) = 0;
    virtual bool UseCashAddrEncoding() const = 0;
};

class GlobalConfig final : public Config
{
public:
    GlobalConfig();
    const CChainParams &GetChainParams() const override;
    void SetCashAddrEncoding(bool) override;
    bool UseCashAddrEncoding() const override;

private:
    bool useCashAddr;
};

// Dummy for subclassing in unittests
class DummyConfig : public Config
{
public:
    const CChainParams &GetChainParams() const override;
    void SetCashAddrEncoding(bool) override {}
    bool UseCashAddrEncoding() const override { return false; }
};

// Temporary woraround.
const Config &GetConfig();

#endif
