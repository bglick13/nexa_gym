// Copyright (c) 2017 The Bitcoin developers
// Copyright (c) 2017-2019 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "guiutiltests.h"
#include "chainparams.h"
#include "config.h"
#include "dstencode.h"
#include "guiutil.h"
#include "receiverequestdialog.h"

namespace {

class UtilCfgDummy : public DummyConfig {
public:
    UtilCfgDummy() : useCashAddr(false) {}
    void SetCashAddrEncoding(bool b) override { useCashAddr = b; }
    bool UseCashAddrEncoding() const override { return useCashAddr; }
    const CChainParams &GetChainParams() const override {
        return Params(CBaseChainParams::NEXTCHAIN);
    }

private:
    bool useCashAddr;
};

} // anon ns

void GUIUtilTests::dummyAddressTest() {
    CChainParams &params = Params(CBaseChainParams::NEXTCHAIN);
    UtilCfgDummy cfg;
    std::string dummyaddr;

    cfg.SetCashAddrEncoding(false);
    dummyaddr = GUIUtil::DummyAddress(params, cfg);
    QVERIFY(!IsValidDestinationString(dummyaddr, params));
    QVERIFY(!dummyaddr.empty());

    cfg.SetCashAddrEncoding(true);
    dummyaddr = GUIUtil::DummyAddress(params, cfg);
    QVERIFY(!IsValidDestinationString(dummyaddr, params));
    QVERIFY(!dummyaddr.empty());
}

void GUIUtilTests::toCurrentEncodingTest() {
    UtilCfgDummy config;

    // garbage in, garbage out
    QVERIFY(ToCurrentEncoding("garbage", config) == "garbage");

    QString cashaddr_pubkey = "nexa:qqjk5f068wpfdhmqh5rnrrqkhryl6lacq5tl8m6qg7";
    QString base58_pubkey = "B7run8V4hf1NJ46UUB2WkHXC8aW6NVz1iR";

    config.SetCashAddrEncoding(true);
    QVERIFY(ToCurrentEncoding(cashaddr_pubkey, config) == cashaddr_pubkey);
    QVERIFY(ToCurrentEncoding(base58_pubkey, config) == cashaddr_pubkey);

    config.SetCashAddrEncoding(false);
    QVERIFY(ToCurrentEncoding(cashaddr_pubkey, config) == base58_pubkey);
    QVERIFY(ToCurrentEncoding(base58_pubkey, config) == base58_pubkey);
}
