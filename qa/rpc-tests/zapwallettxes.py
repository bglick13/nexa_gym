#!/usr/bin/env python3
# Copyright (c) 2014-2015 The Bitcoin Core developers
# Copyright (c) 2015-2017 The Bitcoin Unlimited developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
import test_framework.loginit
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *


class ZapWalletTXesTest (BitcoinTestFramework):

    def setup_chain(self):
        print("Initializing test directory "+self.options.tmpdir)
        initialize_chain_clean(self.options.tmpdir, 3)

    def setup_network(self, split=False):
        self.nodes = start_nodes(3, self.options.tmpdir)
        connect_nodes_full(self.nodes)
        self.is_network_split=False
        self.sync_all()

    def run_test (self):
        print("Mining blocks...")
        self.nodes[0].generate(1)
        self.sync_all()
        self.nodes[1].generate(101)
        self.sync_all()

        assert_equal(self.nodes[0].getbalance(), COINBASE_REWARD)

        txid0 = self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), 100)
        txid1 = self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), 200)
        self.sync_all()
        self.nodes[0].generate(1)
        self.sync_all()

        txid2 = self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), 100)
        txid3 = self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), 200)

        tx0 = self.nodes[0].gettransaction(txid0)
        assert_equal(tx0['txidem'], txid0) #tx0 must be available (confirmed)

        tx1 = self.nodes[0].gettransaction(txid1)
        assert_equal(tx1['txidem'], txid1) #tx1 must be available (confirmed)

        tx2 = self.nodes[0].gettransaction(txid2)
        assert_equal(tx2['txidem'], txid2) #tx2 must be available (unconfirmed)

        tx3 = self.nodes[0].gettransaction(txid3)
        assert_equal(tx3['txidem'], txid3) #tx3 must be available (unconfirmed)

        #restart bitcoind
        self.nodes[0].stop()
        bitcoind_processes[0].wait()
        self.nodes[0] = start_node(0,self.options.tmpdir)

        tx3 = self.nodes[0].gettransaction(txid3)
        assert_equal(tx3['txidem'], txid3) #tx must be available (unconfirmed)

        self.nodes[0].stop()
        bitcoind_processes[0].wait()

        #restart bitcoind with zapwallettxes
        self.nodes[0] = start_node(0,self.options.tmpdir, ["-zapwallettxes=1"])

        assert_raises(JSONRPCException, self.nodes[0].gettransaction, [txid3])
        #there must be an exception because the unconfirmed wallettx0 must be gone by now

        tx0 = self.nodes[0].gettransaction(txid0)
        assert_equal(tx0['txidem'], txid0) #tx0 (confirmed) must still be available because it was confirmed


if __name__ == '__main__':
    ZapWalletTXesTest ().main ()

def Test():
    t = ZapWalletTXesTest()
    t.drop_to_pdb = True
    bitcoinConf = {
        "debug": ["graphene", "blk", "mempool", "net", "req"],
    }
    flags = standardFlags()
    t.main(flags, bitcoinConf, None)
