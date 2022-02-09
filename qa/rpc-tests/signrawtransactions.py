#!/usr/bin/env python3
# Copyright (c) 2015 The Bitcoin Core developers
# Copyright (c) 2015-2017 The Bitcoin Unlimited developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
import test_framework.loginit
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *


class SignRawTransactionsTest(BitcoinTestFramework):
    """Tests transaction signing via RPC command "signrawtransaction"."""

    def setup_chain(self):
        print('Initializing test directory ' + self.options.tmpdir)
        initialize_chain_clean(self.options.tmpdir, 1)

    def setup_network(self, split=False):
        self.nodes = start_nodes(1, self.options.tmpdir)
        self.is_network_split = False

    def successful_signing_test(self):
        """Creates and signs a valid raw transaction with one input.

        Expected results:

        1) The transaction has a complete set of signatures
        2) No script verification error occurred"""
        privKeys = ['cUeKHd5orzT3mz8P9pxyREHfsWtVfgsfDjiZZBcjUBAaGk1BTj7N']

        inputs = [
            # Valid pay-to-pubkey script
            {'outpoint': '9b907ef1e3c26fc71fe4a4b3580bc75264112f95050014157059c736f0202e71',
             'scriptPubKey': '76a91460baa0f494b38ce3c940dea67f3804dc52d1fb9488ac', 'amount': 100.68}
        ]

        outputs = {'mpLQjfK79b7CCV4VMJWEWAj5Mpx8Up5zxB': 10.1}

        rawTx = self.nodes[0].createrawtransaction(inputs, outputs)
        rawTxSigned = self.nodes[0].signrawtransaction(rawTx, inputs, privKeys)

        #### Sign with default forkid
        # 1) The transaction has a complete set of signatures
        assert 'complete' in rawTxSigned
        assert_equal(rawTxSigned['complete'], True)

        # 2) No script verification error occurred
        assert 'errors' not in rawTxSigned

        #### Make sure you can not sign with NOFORKID.
        rawTxSigned_noforkid = self.nodes[0].signrawtransaction(rawTx, inputs, privKeys, "ALL|NOFORKID")

        # 1) The transaction does not have a complete set of signatures
        assert 'complete' in rawTxSigned_noforkid
        assert_equal(rawTxSigned_noforkid['complete'], False)

        # 2) Script verification error should occurred
        assert 'errors' in rawTxSigned_noforkid

        #### Make sure you can sign with schnorr signatures
        rawTxSigned_schnorr = self.nodes[0].signrawtransaction(rawTx, inputs, privKeys, "ALL|FORKID", "1")
        rawTxSigned_schnorr2 = self.nodes[0].signrawtransaction(rawTx, inputs, privKeys, "ALL|FORKID", "schnorr")
        rawTxSigned_schnorr3 = self.nodes[0].signrawtransaction(rawTx, inputs, privKeys, "ALL|FORKID", "SCHNORR")
        assert_equal(rawTxSigned_schnorr, rawTxSigned_schnorr2)
        assert_equal(rawTxSigned_schnorr, rawTxSigned_schnorr3)
        # check that different sig types were used
        # With only 1 sig type, this check is N/A for now: assert_not_equal(rawTxSigned, rawTxSigned_schnorr)

        # 1) The transaction has a complete set of signatures
        assert 'complete' in rawTxSigned_schnorr
        assert_equal(rawTxSigned_schnorr['complete'], True)

        # 2) No script verification error occurred
        assert 'errors' not in rawTxSigned_schnorr


    def script_verification_error_test(self):
        """Creates and signs a raw transaction with valid (vin 0), invalid (vin 1) and one missing (vin 2) input script.

        Expected results:

        3) The transaction has no complete set of signatures
        4) Two script verification errors occurred
        5) Script verification errors have certain properties ("txid", "vout", "scriptSig", "sequence", "error")
        6) The verification errors refer to the invalid (vin 1) and missing input (vin 2)"""
        privKeys = ['cUeKHd5orzT3mz8P9pxyREHfsWtVfgsfDjiZZBcjUBAaGk1BTj7N']

        inputs = [
            # Valid pay-to-pubkey script
            {'outpoint': '9b907ef1e3c26fc71fe4a4b3580bc75264112f95050014157059c736f0202e71', 'amount': 100.68},
            # Invalid script
            {'outpoint': '5b8673686910442c644b1f4993d8f7753c7c8fcb5c87ee40d56eaeef25204547', 'amount': 100.68},
            # Missing scriptPubKey
            {'outpoint': '8b907ef1e3c26fc71fe4a4b3580bc75264112f95050014157059c736f0202e71', 'amount': 100.68},
        ]

        scripts = [
            # Valid pay-to-pubkey script
            {'outpoint': '9b907ef1e3c26fc71fe4a4b3580bc75264112f95050014157059c736f0202e71', 'amount':100.68,
             'scriptPubKey': '76a91460baa0f494b38ce3c940dea67f3804dc52d1fb9488ac'},
            # Invalid script
            {'outpoint': '5b8673686910442c644b1f4993d8f7753c7c8fcb5c87ee40d56eaeef25204547', 'amount':100.68,
             'scriptPubKey': 'badbadbadbad'}
        ]

        outputs = {'mpLQjfK79b7CCV4VMJWEWAj5Mpx8Up5zxB': 10.1}

        rawTx = self.nodes[0].createrawtransaction(inputs, outputs)
        rawTxSigned = self.nodes[0].signrawtransaction(rawTx, scripts, privKeys)

        # 3) The transaction has no complete set of signatures
        assert 'complete' in rawTxSigned
        assert_equal(rawTxSigned['complete'], False)

        # 4) Two script verification errors occurred
        assert 'errors' in rawTxSigned
        assert_equal(len(rawTxSigned['errors']), 2)

        # 5) Script verification errors have certain properties
        assert 'outpoint' in rawTxSigned['errors'][0]
        assert 'satisfierScript' in rawTxSigned['errors'][0]
        assert 'sequence' in rawTxSigned['errors'][0]
        assert 'error' in rawTxSigned['errors'][0]

        # 6) The verification errors refer to the invalid (vin 1) and missing input (vin 2)
        assert_equal(rawTxSigned['errors'][0]['outpoint'], inputs[1]['outpoint'])
        assert_equal(rawTxSigned['errors'][1]['outpoint'], inputs[2]['outpoint'])

        # now run the same test with schnorr
        rawTxSigned2 = self.nodes[0].signrawtransaction(rawTx, scripts, privKeys, "ALL|FORKID", "1")
        # check that different sig types were used
        # With only 1 sig type, this check is N/A for now: assert_not_equal(rawTxSigned, rawTxSigned2)

        # 3) The transaction has no complete set of signatures
        assert 'complete' in rawTxSigned2
        assert_equal(rawTxSigned2['complete'], False)

        # 4) Two script verification errors occurred
        assert 'errors' in rawTxSigned2
        assert_equal(len(rawTxSigned2['errors']), 2)

        # 5) Script verification errors have certain properties
        assert 'outpoint' in rawTxSigned2['errors'][0]
        assert 'satisfierScript' in rawTxSigned2['errors'][0]
        assert 'sequence' in rawTxSigned2['errors'][0]
        assert 'error' in rawTxSigned2['errors'][0]

        # 6) The verification errors refer to the invalid (vin 1) and missing input (vin 2)
        assert_equal(rawTxSigned2['errors'][0]['outpoint'], inputs[1]['outpoint'])
        assert_equal(rawTxSigned2['errors'][1]['outpoint'], inputs[2]['outpoint'])

    def run_test(self):
        self.successful_signing_test()
        self.script_verification_error_test()


if __name__ == '__main__':
    SignRawTransactionsTest().main()

def Test():
    t = SignRawTransactionsTest()
    t.drop_to_pdb = True
    # install ctrl-c handler
    #import signal, pdb
    #signal.signal(signal.SIGINT, lambda sig, stk: pdb.Pdb().set_trace(stk))
    bitcoinConf = {
        "debug": ["net", "blk", "thin", "mempool", "req", "bench", "evict"],
        "blockprioritysize": 2000000  # we don't want any transactions rejected due to insufficient fees...
    }
    flags = standardFlags()
    t.main(flags, bitcoinConf, None)
