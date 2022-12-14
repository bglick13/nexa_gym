#!/usr/bin/env python3
# Copyright (c) 2016 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import time
from test_framework.mininode import *
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *
from test_framework.blocktools import create_block, create_coinbase, getAncHash
from test_framework.siphash import siphash256
from test_framework.script import CScript, OP_TRUE, OP_RETURN

SYNC_TIMEOUT = 10

'''
CompactBlocksTest -- test compact blocks (BIP 152)
'''


# TestNode: A peer we use to send messages to bitcoind, and store responses.
class TestNode(SingleNodeConnCB):
    def __init__(self):
        SingleNodeConnCB.__init__(self)
        self.last_sendcmpct = None
        self.last_headers = None
        self.last_inv = None
        self.last_cmpctblock = None
        self.block_announced = False
        self.last_getdata = None
        self.last_getblocktxn = None
        self.last_block = None
        self.last_blocktxn = None
        self.sleep_time = 0.05

    def on_sendcmpct(self, conn, message):
        self.last_sendcmpct = message

    def on_block(self, conn, message):
        self.last_block = message

    def on_cmpctblock(self, conn, message):
        self.last_cmpctblock = message
        self.block_announced = True

    def on_headers(self, conn, message):
        self.last_headers = message
        self.block_announced = True

    def on_inv(self, conn, message):
        self.last_inv = message
        self.block_announced = True

    def on_getdata(self, conn, message):
        self.last_getdata = message

    def on_getblocktxn(self, conn, message):
        self.last_getblocktxn = message

    def on_blocktxn(self, conn, message):
        self.last_blocktxn = message

    # Requires caller to hold mininode_lock
    def received_block_announcement(self):
        return self.block_announced

    def clear_block_announcement(self):
        with mininode_lock:
            self.block_announced = False
            self.last_inv = None
            self.last_headers = None
            self.last_cmpctblock = None

    def get_headers(self, locator, hashstop):
        msg = msg_getheaders()
        msg.locator.vHave = locator
        msg.hashstop = hashstop
        self.connection.send_message(msg)

    def send_header_for_blocks(self, new_blocks):
        headers_message = msg_headers()
        headers_message.headers = [CBlockHeader(b) for b in new_blocks]
        self.send_message(headers_message)

    # Syncing helpers
    def sync(self, test_function, timeout=60):
        while timeout > 0:
            with mininode_lock:
                if test_function():
                    return

            time.sleep(self.sleep_time)
            timeout -= self.sleep_time
        raise AssertionError("Sync failed to complete")

    def sync_getdata(self, hash_list, timeout=60):
        while timeout > 0:
            with mininode_lock:
                x = self.last_getdata

            #Check whether any getdata responses are in the hash list and
            #if so remove them.
            for y in hash_list:
                if (str(x).find(y) > 0):
                    hash_list.remove(y)
            if hash_list == []:
                return

            time.sleep(self.sleep_time)
            timeout -= self.sleep_time
        raise AssertionError("Sync getdata failed to complete")

    def wait_for_getdata(self, hash_list, timeout=60):
        if hash_list == []:
            return

        self.sync_getdata(hash_list, timeout)
        return

    def check_mempools(self, tx_list, peer, timeout=10):
       success = False
       while success is False:
           success = True
           txpool = peer.getrawtxpool(False, "id")
           orphanpool = peer.getraworphanpool()

           for tx in tx_list:
               txid = tx.GetRpcHexId()
               if txid not in txpool and txid not in orphanpool:
                    success = False

           time.sleep(self.sleep_time)
           timeout -= self.sleep_time

           if success == True:
               return
           if  timeout <= 0:
               raise AssertionError("Sync getdata failed to complete")

    def check_mempool(self, tx_list, peer, timeout=10):
       success = False
       while success is False:
           success = True
           txpool = peer.getrawtxpool(False, "id")

           for tx in tx_list:
                if tx.GetRpcHexId() not in txpool:
                    success = False

           time.sleep(self.sleep_time)
           timeout -= self.sleep_time

           if success == True:
               return
           if  timeout <= 0:
               raise AssertionError("Sync getdata failed to complete")

class CompactBlocksTest(BitcoinTestFramework):
    def __init__(self):
        super().__init__()
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.utxos = []

    def setup_network(self):
        self.nodes = []
        self.nodes = start_nodes(self.num_nodes, self.options.tmpdir,
            [["-debug=net",
              "-debug=thin",
              "-debug=cmpctblocks",
              "-debug=mempool",
              "-net.msgHandlerThreads=1",
              "-test.parallel=0"]])


    def build_block_on_tip(self):
        height = self.nodes[0].getblockcount()
        tip = self.nodes[0].getbestblockhash()
        tipHdr = self.nodes[0].getblockheader(tip)
        mtp = tipHdr['mediantime']
        chainwork = int(tipHdr["chainwork"], 16)
        block = create_block(uint256_from_bigendian(tip), height+1, chainwork+2, create_coinbase(height + 1), getAncHash(height+1, self.nodes[0]), mtp + 1)
        block.solve()
        return block

    # Create 10 more anyone-can-spend utxo's for testing.
    def make_utxos(self):
        block = self.build_block_on_tip()
        block.update_fields()
        block.solve()
        block.calc_hash()
        self.test_node.send_and_ping(msg_block(block))
        waitFor(SYNC_TIMEOUT, lambda: self.nodes[0].getbestblockhash() == block.hash)
        self.nodes[0].generate(100)

        total_value = block.vtx[0].vout[0].nValue
        out_value = decimal.Decimal(total_value) / (COIN*100)
        tx = CTransaction()
        tx.vin.append(block.vtx[0].SpendOutput(0, CScript([OP_TRUE])))
        for i in range(64):
            tx.vout.append(TxOut(0,out_value, CScript([OP_TRUE])))
        tx.rehash()

        block2 = self.build_block_on_tip()
        block2.vtx.append(tx)
        block2.update_fields()
        block2.solve()
        block2.calc_hash()
        self.test_node.send_and_ping(msg_block(block2))

        waitFor(SYNC_TIMEOUT, lambda: self.nodes[0].getbestblockhash() == block2.hash)
        self.utxos.extend([tx.SpendOutput(i) for i in range(10)])
        return

    # Test "sendcmpct":
    # - No compact block announcements or getdata(MSG_CMPCT_BLOCK) unless
    #   sendcmpct is sent.
    # - If sendcmpct is sent with version > 1, the message is ignored.
    # - If sendcmpct is sent with boolean 0, then block announcements are not
    #   made with compact blocks.
    # - If sendcmpct is then sent with boolean 1, then new block announcements
    #   are made with compact blocks.
    def test_sendcmpct(self):
        print("Testing SENDCMPCT p2p message... ")

        # Make sure we get a version 0 SENDCMPCT message from our peer
        def received_sendcmpct():
            return (self.test_node.last_sendcmpct is not None)
        got_message = wait_until(received_sendcmpct, timeout=30)
        assert(got_message)
        assert_equal(self.test_node.last_sendcmpct.version, 1)

        tip = int(self.nodes[0].getbestblockhash(), 16)

        def check_announcement_of_new_block(node, peer, predicate):
            self.test_node.clear_block_announcement()
            node.generate(1)
            got_message = wait_until(peer.received_block_announcement, timeout=30)
            assert(got_message)
            with mininode_lock:
                assert(predicate)

        # We shouldn't get any block announcements via cmpctblock yet.
        check_announcement_of_new_block(self.nodes[0], self.test_node, lambda: self.test_node.last_cmpctblock is None)

        # Try one more time, this time after requesting headers.
        self.test_node.clear_block_announcement()
        self.test_node.get_headers(locator=[tip], hashstop=0)
        wait_until(self.test_node.received_block_announcement, timeout=30)
        self.test_node.clear_block_announcement()

        check_announcement_of_new_block(self.nodes[0], self.test_node, lambda: self.test_node.last_cmpctblock is None and self.test_node.last_inv is not None)

        # Now try a SENDCMPCT message with too-high version
        sendcmpct = msg_sendcmpct()
        sendcmpct.version = 2
        self.test_node.send_message(sendcmpct)

        check_announcement_of_new_block(self.nodes[0], self.test_node, lambda: self.test_node.last_cmpctblock is None)

        # Now try a SENDCMPCT message with valid version, but announce=False
        self.test_node.send_message(msg_sendcmpct())
        check_announcement_of_new_block(self.nodes[0], self.test_node, lambda: self.test_node.last_cmpctblock is None)

        # Finally, try a SENDCMPCT message with announce=True
        sendcmpct.version = 1
        sendcmpct.announce = True
        self.test_node.send_message(sendcmpct)
        check_announcement_of_new_block(self.nodes[0], self.test_node, lambda: self.test_node.last_cmpctblock is not None)

        # Try one more time
        check_announcement_of_new_block(self.nodes[0], self.test_node, lambda: self.test_node.last_cmpctblock is not None)

        # Try one more time, after turning on sendheaders
        self.test_node.send_message(msg_sendheaders())
        check_announcement_of_new_block(self.nodes[0], self.test_node, lambda: self.test_node.last_cmpctblock is not None)

        # Now turn off announcements
        sendcmpct.announce = False
        check_announcement_of_new_block(self.nodes[0], self.test_node, lambda: self.test_node.last_cmpctblock is None and self.test_node.last_headers is not None)

    def test_invalid_cmpctblock_message(self):
        print("Testing invalid index in cmpctblock message...")
        self.nodes[0].generate(101)
        block = self.build_block_on_tip()

        cmpct_block = P2PHeaderAndShortIDs()
        cmpct_block.header = CBlockHeader(block)
        cmpct_block.prefilled_txn_length = 1
        # This index will be too high
        prefilled_txn = PrefilledTransaction(1, block.vtx[0])
        cmpct_block.prefilled_txn = [prefilled_txn]
        self.test_node.send_and_ping(msg_cmpctblock(cmpct_block))
        waitFor(30, lambda: int(self.nodes[0].getbestblockhash(), 16) == block.hashPrevBlock)

    # Compare the generated shortids to what we expect based on BIP 152, given
    # bitcoind's choice of nonce.
    def test_compactblock_construction(self):
        print("Testing compactblock headers and shortIDs are correct...")

        # Generate a bunch of transactions.
        self.nodes[0].generate(101)
        num_transactions = 25
        address = self.nodes[0].getnewaddress()
        for i in range(num_transactions):
            self.nodes[0].sendtoaddress(address, 100000.01)
        time.sleep(1)

        # Now mine a block, and look at the resulting compact block.
        self.test_node.clear_block_announcement()
        block_hash = int(self.nodes[0].generate(1)[0], 16)

        # Store the raw block in our internal format.
        block = FromHex(CBlock(), self.nodes[0].getblock("%02x" % block_hash, False))
        [tx.rehash() for tx in block.vtx]
        block.rehash()

        # Don't care which type of announcement came back for this test; just
        # request the compact block if we didn't get one yet.
        wait_until(self.test_node.received_block_announcement, timeout=30)

        with mininode_lock:
            if self.test_node.last_cmpctblock is None:
                self.test_node.clear_block_announcement()
                inv = CInv(4, block_hash)  # 4 == "CompactBlock"
                self.test_node.send_message(msg_getdata([inv]))

        wait_until(self.test_node.received_block_announcement, timeout=30)

        # Now we should have the compactblock
        header_and_shortids = None
        with mininode_lock:
            assert(self.test_node.last_cmpctblock is not None)
            # Convert the on-the-wire representation to absolute indexes
            header_and_shortids = HeaderAndShortIDs(self.test_node.last_cmpctblock.header_and_shortids)

        # Check that we got the right block!
        header_and_shortids.header.calc_hash()
        assert_equal(header_and_shortids.header.gethash(), block_hash)

        # Make sure the prefilled_txn appears to have included the coinbase
        assert(len(header_and_shortids.prefilled_txn) >= 1)
        assert_equal(header_and_shortids.prefilled_txn[0].index, 0)

        # Check that all prefilled_txn entries match what's in the block.
        for entry in header_and_shortids.prefilled_txn:
            entry.tx.rehash()
            assert_equal(entry.tx.GetId(), block.vtx[entry.index].GetId())

        # Check that the cmpctblock message announced all the transactions.
        assert_equal(len(header_and_shortids.prefilled_txn) + len(header_and_shortids.shortids), len(block.vtx))

        # And now check that all the shortids are as expected as well.
        # Determine the siphash keys to use.
        [k0, k1] = header_and_shortids.get_siphash_keys()

        index = 0
        while index < len(block.vtx):
            if (len(header_and_shortids.prefilled_txn) > 0 and
                    header_and_shortids.prefilled_txn[0].index == index):
                # Already checked prefilled transactions above
                header_and_shortids.prefilled_txn.pop(0)
            else:
                shortid = calculate_shortid(k0, k1, block.vtx[index].GetIdAsInt())
                assert_equal(shortid, header_and_shortids.shortids[0])
                header_and_shortids.shortids.pop(0)
            index += 1

    # Test that bitcoind requests compact blocks when we announce new blocks
    # via header or inv, and that responding to getblocktxn causes the block
    # to be successfully reconstructed.
    def test_compactblock_requests(self):
        print("Testing compactblock requests... ")

        # Try announcing a block with an inv or header, expect a compactblock
        # request
        for announce in ["header"]:
            block = self.build_block_on_tip()
            with mininode_lock:
                self.test_node.last_getdata = None

            if announce == "inv":
                self.test_node.send_message(msg_inv([CInv(2, block.sha256)]))
            else:
                self.test_node.send_header_for_blocks([block])
            waitFor(30, lambda: self.test_node.last_getdata is not None)
            assert_equal(len(self.test_node.last_getdata.inv), 1)
            assert_equal(self.test_node.last_getdata.inv[0].type, 4)
            assert_equal(self.test_node.last_getdata.inv[0].hash, block.gethash())

            # Send back a compactblock message that omits the coinbase
            comp_block = HeaderAndShortIDs()
            comp_block.header = CBlockHeader(block)
            comp_block.nonce = 0
            [k0, k1] = comp_block.get_siphash_keys()
            coinbase_hash = block.vtx[0].GetIdAsInt()
            comp_block.shortids = [calculate_shortid(k0, k1, coinbase_hash)]
            self.test_node.send_and_ping(msg_cmpctblock(comp_block.to_p2p()))
            waitFor(30, lambda: int(self.nodes[0].getbestblockhash(), 16), block.hashPrevBlock)
            # Expect a getblocktxn message.
            with mininode_lock:
                waitFor(30, lambda: self.test_node.last_getblocktxn is not None)
                absolute_indexes = self.test_node.last_getblocktxn.block_txn_request.to_absolute()
            assert_equal(absolute_indexes, [0])  # should be a coinbase request

            # Send the coinbase, and verify that the tip advances.
            msg = msg_blocktxn()
            msg.block_transactions.blockhash = block.gethash()
            msg.block_transactions.transactions = [block.vtx[0]]
            self.test_node.send_and_ping(msg)
            waitFor(30, lambda: int(self.nodes[0].getbestblockhash(), 16), block.gethash())

    # Create a chain of transactions from given utxo, and add to a new block.
    def build_block_with_transactions(self, utxo, num_transactions):
        block = self.build_block_on_tip()

        for i in range(num_transactions):
            tx = CTransaction()
            tx.vin.append(utxo)
            tx.vout.append(CTxOut(utxo.amount - 1000, CScript([OP_TRUE])))
            padding = 1 << 8 * 100
            tx.vout.append(CTxOut(0, CScript([padding, OP_RETURN])))
            tx.rehash()
            utxo = tx.SpendOutput(0)
            block.vtx.append(tx)

        ordered_txs = block.vtx
        block.vtx = [block.vtx[0]] + sorted(block.vtx[1:], key=lambda tx: tx.GetRpcHexId())
        block.update_fields()
        block.solve()
        block.rehash()
        return block, ordered_txs

    def announce_new_block(self, block):
        with mininode_lock:
            self.test_node.send_header_for_blocks([block])

        self.test_node.wait_for_getdata([block.hash], timeout=30)

    # Test that we only receive getblocktxn requests for transactions that the
    # node needs, and that responding to them causes the block to be
    # reconstructed.
    def test_getblocktxn_requests(self):
        print("Testing getblocktxn requests...")


        # First try announcing compactblocks that won't reconstruct, and verify
        # that we receive getblocktxn messages back.
        utxo = self.utxos.pop(0)

        block, ordered_txs = self.build_block_with_transactions(utxo, 5)
        self.announce_new_block(block)
        self.utxos.append([ordered_txs[-1].GetId(), 0, ordered_txs[-1].vout[0].nValue])

        comp_block = HeaderAndShortIDs()
        comp_block.initialize_from_block(block)

        self.test_node.last_getblocktxn = None
        self.test_node.send_and_ping(msg_cmpctblock(comp_block.to_p2p()))
        with mininode_lock:
            waitFor(30, lambda: self.test_node.last_getblocktxn is not None)
            absolute_indices = self.test_node.last_getblocktxn.block_txn_request.to_absolute()
        expected_indices = []
        for i in [1, 2, 3, 4, 5]:
            expected_indices.append(block.vtx.index(ordered_txs[i]))
        assert_equal(absolute_indices, sorted(expected_indices))
        msg = msg_blocktxn()
        msg.block_transactions = BlockTransactions(block.gethash(), block.vtx[1:])
        self.test_node.send_and_ping(msg)
        waitFor(SYNC_TIMEOUT, lambda: self.nodes[0].getbestblockhash() == block.gethashhex())


        utxo = self.utxos.pop(0)
        block, ordered_txs = self.build_block_with_transactions(utxo, 5)
        self.announce_new_block(block)
        self.utxos.append([ordered_txs[-1].GetId(), 0, ordered_txs[-1].vout[0].nValue])

        # Now try interspersing the prefilled transactions
        comp_block.initialize_from_block(block, prefill_list=[0, 1, 5])
        self.test_node.send_and_ping(msg_cmpctblock(comp_block.to_p2p()))
        with mininode_lock:
            waitFor(SYNC_TIMEOUT, lambda: self.test_node.last_getblocktxn is not None)
            absolute_indices = self.test_node.last_getblocktxn.block_txn_request.to_absolute()
        assert_equal(absolute_indices, [2,3,4])
        msg.block_transactions = BlockTransactions(block.gethash(), block.vtx[2:5])
        self.test_node.send_and_ping(msg)
        waitFor(SYNC_TIMEOUT, lambda: self.nodes[0].getbestblockhash() == block.gethashhex())


        # Now try giving two transactions ahead of time.
        utxo = self.utxos.pop(0)
        block, ordered_txs = self.build_block_with_transactions(utxo, 10)
        self.announce_new_block(block)
        self.utxos.append([ordered_txs[-1].GetId(), 0, ordered_txs[-1].vout[0].nValue])
        self.test_node.send_and_ping(msg_tx(block.vtx[1]))
        self.test_node.send_and_ping(msg_tx(block.vtx[7]))
        self.test_node.check_mempools([block.vtx[1], block.vtx[7]], self.nodes[0], timeout=SYNC_TIMEOUT)

        # Prefill 4 out of the 10 transactions, and verify that only the one
        # that was not in the txpool is requested.
        comp_block.initialize_from_block(block, prefill_list=[0, 2, 3, 4])
        self.test_node.send_and_ping(msg_cmpctblock(comp_block.to_p2p()))
        with mininode_lock:
            waitFor(30, lambda: self.test_node.last_getblocktxn is not None)
            absolute_indices = self.test_node.last_getblocktxn.block_txn_request.to_absolute()
        assert_equal(absolute_indices, [5, 6, 8, 9, 10])

        # send back the re-request but also include one tx "block.vtx[7]" which is in the peer's mempool.
        msg.block_transactions = BlockTransactions(block.gethash(), [block.vtx[5], block.vtx[6], block.vtx[7], block.vtx[8], block.vtx[9], block.vtx[10]])
        self.test_node.send_and_ping(msg)
        waitFor(SYNC_TIMEOUT, lambda: int(self.nodes[0].getbestblockhash(), 16) == block.gethash())


        # Now provide all transactions to the node before the block is
        # announced and verify reconstruction happens immediately.
        utxo = self.utxos.pop(0)
        block, ordered_txs = self.build_block_with_transactions(utxo, 10)
        self.announce_new_block(block)
        self.utxos.append([ordered_txs[-1].GetId(), 0, ordered_txs[-1].vout[0].nValue])
        for tx in ordered_txs[1:]:
            self.test_node.send_and_ping(msg_tx(tx))

        # Make sure all transactions were accepted.
        self.test_node.check_mempool(ordered_txs[1:], self.nodes[0], timeout=30)

        # Clear out last request.
        with mininode_lock:
            self.test_node.last_getblocktxn = None

        # Send compact block
        comp_block.initialize_from_block(block, prefill_list=[0])
        self.test_node.send_and_ping(msg_cmpctblock(comp_block.to_p2p()))
        time.sleep(1)
        with mininode_lock:
            # Shouldn't have gotten a request for any transaction
            assert(self.test_node.last_getblocktxn is None)
        # Tip should have updated
        assert_equal(int(self.nodes[0].getbestblockhash(), 16), block.gethash())

        # Now provide all transactions as prefilled and
        # and verify reconstruction happens immediately.
        utxo = self.utxos.pop(0)
        block, ordered_txs = self.build_block_with_transactions(utxo, 10)
        self.announce_new_block(block)
        self.utxos.append([ordered_txs[-1].GetId(), 0, ordered_txs[-1].vout[0].nValue])

        # Clear out last request.
        with mininode_lock:
            self.test_node.last_getblocktxn = None

        # Send compact block
        comp_block.initialize_from_block(block, prefill_list=[0, 1, 2, 3, 4, 5, 6, 7, 8, 9 ,10])
        self.test_node.send_and_ping(msg_cmpctblock(comp_block.to_p2p()))
        time.sleep(1)
        with mininode_lock:
            # Shouldn't have gotten a request for any transaction
            assert(self.test_node.last_getblocktxn is None)
        # Tip should have updated
        assert_equal(int(self.nodes[0].getbestblockhash(), 16), block.gethash())

        # Now provide none of the transactions as prefilled (not even the coinbase) and
        # and verify a request for transactions.
        utxo = self.utxos.pop(0)
        block, ordered_txs = self.build_block_with_transactions(utxo, 3)
        self.announce_new_block(block)
        self.utxos.append([ordered_txs[-1].GetId(), 0, ordered_txs[-1].vout[0].nValue])

        # Clear out last request.
        with mininode_lock:
            self.test_node.last_getblocktxn = None

        # Send compact block
        comp_block.initialize_from_block(block, prefill_list=[])
        self.test_node.send_and_ping(msg_cmpctblock(comp_block.to_p2p()))

        with mininode_lock:
            waitFor(30, lambda: self.test_node.last_getblocktxn is not None)
            absolute_indices = self.test_node.last_getblocktxn.block_txn_request.to_absolute()
        assert_equal(absolute_indices, [0, 1, 2, 3])

        # send back the re-request with all transactions including coinbase.
        msg.block_transactions = BlockTransactions(block.gethash(), [block.vtx[0], block.vtx[1],block.vtx[2],block.vtx[3]])
        self.test_node.send_and_ping(msg)
        waitFor(SYNC_TIMEOUT, lambda: int(self.nodes[0].getbestblockhash(), 16) == block.gethash())


    # Incorrectly responding to a getblocktxn shouldn't cause the block to be permanently
    # failed but will cause the node to be disconected due to a merkle root check failure.
    def test_incorrect_blocktxn_response(self):
        print("Testing handling of incorrect blocktxn responses...")
        if (len(self.utxos) == 0):
            self.make_utxos()
        utxo = self.utxos.pop(0)

        block, ordered_txs = self.build_block_with_transactions(utxo, 10)
        self.announce_new_block(block)
        self.utxos.append([ordered_txs[-1].GetId(), 0, ordered_txs[-1].vout[0].nValue])
        # Relay the first 5 transactions from the block in advance
        for tx in ordered_txs[1:6]:
            self.test_node.send_message(msg_tx(tx))

        # Make sure all transactions were accepted in either the tx pool or orphan pool.
        self.test_node.check_mempools(ordered_txs[1:6], self.nodes[0], timeout=SYNC_TIMEOUT)

        # Send compact block
        comp_block = HeaderAndShortIDs()
        comp_block.initialize_from_block(block, prefill_list=[0])
        self.test_node.send_and_ping(msg_cmpctblock(comp_block.to_p2p()))
        absolute_indices = []
        with mininode_lock:
            waitFor(SYNC_TIMEOUT, lambda: self.test_node.last_getblocktxn is not None)
            absolute_indices = self.test_node.last_getblocktxn.block_txn_request.to_absolute()
        expected_indices = []
        for i in [6, 7, 8, 9, 10]:
            expected_indices.append(block.vtx.index(ordered_txs[i]))
        assert_equal(absolute_indices, sorted(expected_indices))

        # Now give an incorrect response.
        # Note that it's possible for bitcoind to be smart enough to know we're
        # lying, since it could check to see if the shortid matches what we're
        # sending, and eg disconnect us for misbehavior.  If that behavior
        # change were made, we could just modify this test by having a
        # different peer provide the block further down, so that we're still
        # verifying that the block isn't marked bad permanently. This is good
        # enough for now.

        msg = msg_blocktxn()
        msg.block_transactions = BlockTransactions(block.gethash(), [ordered_txs[5]] + ordered_txs[7:])
        self.test_node.send_and_ping(msg)

        # Tip should not have updated
        waitFor(30, lambda: int(self.nodes[0].getbestblockhash(), 16) == block.hashPrevBlock)

        # We should NOT receive a getdata request. BU differs from ABC/Core in that if
        # a peer does not give us the all the txns we requested then we ban them due to
        # the merkle root check failing.
        waitFor(30, lambda: self.test_node.last_getdata is not None)

        assert_equal(len(self.test_node.last_getdata.inv), 1)
        assert_equal(self.test_node.last_getdata.inv[0].type, 4) # previous getdata

    def run_test(self):
        # Setup the p2p connections and start up the network thread.
        self.test_node = TestNode()

        connections = []
        connections.append(NodeConn('127.0.0.1', p2p_port(0), self.nodes[0], self.test_node))
        self.test_node.add_connection(connections[0])

        NetworkThread().start()  # Start up network handling in another thread

        # Test logic begins here
        self.test_node.send_message(msg_extversion(), True)
        self.test_node.wait_for_verack()

        # We will need UTXOs to construct transactions in later tests.
        self.make_utxos()

        self.test_sendcmpct()
        self.test_compactblock_construction()
        self.test_compactblock_requests()
        self.test_getblocktxn_requests()
        self.test_invalid_cmpctblock_message()
        self.test_incorrect_blocktxn_response()
        print("Expect this error to be written to stderr: 'EXCEPTION: St16invalid_argument'")


if __name__ == '__main__':
    CompactBlocksTest().main()

def Test():
    t = CompactBlocksTest()
    t.drop_to_pdb = True
    bitcoinConf = {
        "debug": ["all","-libevent"]
    }
    logging.getLogger().setLevel(logging.INFO)
    flags = standardFlags()
    t.main(flags, bitcoinConf, None)
