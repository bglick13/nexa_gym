# Copyright (c) 2019 The Bitcoin Unlimited developers

from test_framework.loginit import logging
import socket
import json
import asyncio
from . import cashaddr
from .script import *
from test_framework.connectrum.client import StratumClient
from test_framework.connectrum.svr_info import ServerInfo
from test_framework.util import waitFor, rpcHexToUint256
from test_framework.test_framework import BitcoinTestFramework
from test_framework.mininode import (
    P2PDataStore,
    NodeConn,
    NetworkThread,
)
from test_framework.util import assert_equal, p2p_port
from test_framework.blocktools import create_coinbase, create_block, getAncHash, ancestorHeight
from test_framework.portseed import electrum_rpc_port
import time

ERROR_CODE_INVALID_REQUEST = -32600
ERROR_CODE_METHOD_NOT_FOUND = -32601
ERROR_CODE_INVALID_PARAMS = -32602
ERROR_CODE_INTERNAL_ERROR = -32603
ERROR_CODE_NOT_FOUND = -32004
ERROR_CODE_TIMEOUT = -32005

class ElectrumTestFramework(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [bitcoind_electrum_args()]

        # Cached to speed up mining
        self.hash_at_height = {}

    def bootstrap_p2p(self):
        """Add a P2P connection to the node."""
        self.p2p = P2PDataStore()
        self.connection = NodeConn('127.0.0.1', p2p_port(0), self.nodes[0], self.p2p)
        self.p2p.add_connection(self.connection)
        NetworkThread().start()
        self.p2p.wait_for_verack()
        assert(self.p2p.connection.state == "connected")

    def mine_blocks(self, n, num_blocks, txns = None):
        """
        Mine a block without using bitcoind
        """
        genesis_block_hash = rpcHexToUint256(n.getblockheader(0)['hash'])

        prev = n.getblockheader(n.getbestblockhash())
        prev_height = prev['height']
        prev_hash = prev['hash']
        prev_time = max(prev['time'] + 1, int(time.time()))
        prev_chainwork = rpcHexToUint256(prev['chainwork'])

        blocks = [ ]
        for i in range(num_blocks):
            height = prev_height + 1
            ancestor_height = ancestorHeight(height)
            if ancestor_height == 0:
                ancestor_hash = genesis_block_hash
            else:
                ancestor_hash = self.hash_at_height.get(ancestor_height, None)
                if ancestor_hash is None:
                    ancestor_hash = getAncHash(ancestor_height, n)

            coinbase = create_coinbase(height)
            b = create_block(
                    hashprev = prev_hash,
                    chainwork = prev_chainwork + 2,
                    height = height,
                    coinbase = coinbase,
                    hashAncestor = ancestor_hash,
                    txns = txns,
                    nTime = prev_time + 1)
            txns = None
            b.solve()
            blocks.append(b)

            prev_time = b.nTime
            prev_height += 1
            prev_hash = b.gethash()
            prev_chainwork = b.chainWork
            self.hash_at_height[height] = b.gethash()

        self.p2p.send_blocks_and_test(blocks, n)
        assert_equal(blocks[-1].hash, n.getbestblockhash())
        self.sync_height()

        # Return coinbases for spending later
        return [b.vtx[0] for b in blocks]

    def sync_height(self, n = None):
        if n is None:
            n = self.nodes[0]
        sync_electrum_height(n)

    def wait_for_mempool_count(self, n = None, *, count, timeout = 10):
        if n is None:
            n = self.nodes[0]
        wait_for_electrum_mempool(n, count = count, timeout = timeout)

def compare(node, key, expected, is_debug_data = False):
    info = node.getelectruminfo()
    if is_debug_data:
        info = info['debuginfo']
        key = "rostrum_" + key
    logging.debug("expecting %s == %s from %s", key, expected, info)
    if key not in info:
        return False
    return info[key] == expected

def bitcoind_electrum_args():
    return ["-electrum=1", "-debug=electrum", "-debug=rpc",
            "-electrum.rawarg=--cashaccount-activation-height=1",
            "-electrum.rawarg=--wait-duration-secs=1",
            ]

class TestClient(StratumClient):
    is_connected = False

    def connection_lost(self, protocol):
        self.is_connected = False
        super().connection_lost(protocol)

class ElectrumConnection:
    def __init__(self, loop = None):
        self.cli = TestClient(loop)

    async def connect(self, node_index = 0):
        connect_timeout = 30
        import time
        start = time.time()

        while True:
            try:
                await self.cli.connect(ServerInfo(None,
                    ip_addr = "127.0.0.1", ports = electrum_rpc_port(n = node_index)))
                self.cli.is_connected = True
                break

            except Exception as e:
                if time.time() >= (start + connect_timeout):
                    raise Exception("Failed to connect to electrum server. Error '{}'".format(e))

            time.sleep(1)

    def disconnect(self):
        self.cli.close()

    async def call(self, method, *args):
        if not self.cli.is_connected:
            raise Exception("not connected")
        return await self.cli.RPC(method, *args)

    async def subscribe(self, method, *args):
        if not self.cli.is_connected:
            raise Exception("not connected")
        future, queue = self.cli.subscribe(method, *args)
        result = await future
        return result, queue

    def is_connected(self):
        return self.cli.is_connected

def script_to_scripthash(script):
    import hashlib
    scripthash = hashlib.sha256(script).digest()

    # Electrum wants little endian
    scripthash = bytearray(scripthash)
    scripthash.reverse()

    return scripthash.hex()


def sync_electrum_height(node, timeout = 10):
    waitFor(timeout, lambda: compare(node, "index_height", node.getblockcount()))

def wait_for_electrum_mempool(node, *, count, timeout = 10):
    try:
        waitFor(timeout, lambda: compare(node, "mempool_count", count, True))
    except Exception as e:
        print("Waited for {} txs, had {}".format(count, node.getelectruminfo()['debuginfo']['rostrum_mempool_count']))
        raise

def get_txid_from_idem(n, txidem):
    return n.getrawtransaction(txidem, True)['txid']

"""
Asserts that function call throw a electrum error, optionally testing for
the contents of the error.
"""
async def assert_response_error(call,
        error_code = None, error_string = None):
    from test_framework.connectrum.exc import ElectrumErrorResponse
    try:
        await call()
        raise AssertionError("assert_electrum_error: Error was not thrown.")
    except ElectrumErrorResponse as exception:
        res = exception.response

        if error_code is not None:
            if not 'code' in res:
                raise AssertionError(
                    "assert_response_error: Error code is missing in response")

            if res['code'] != error_code:
                raise AssertionError((
                    "assert_response_error: Expected error code {}, "
                    "got {} (Full response: {})".format(
                    error_code, res['code'], str(exception))))

        if error_string is not None:
            if not 'message' in res:
                raise AssertionError(
                    "assert_response_error: Error message is missing in response")

            if error_string not in res['message']:
                raise AssertionError((
                    "assert_response_error: Expected error string '{}', "
                    "not found in '{}' (Full response: {})").format(
                    error_string, res['message'], str(exception)))
