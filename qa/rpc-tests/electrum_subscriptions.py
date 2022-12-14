#!/usr/bin/env python3
# Copyright (c) 2019-2020 The Bitcoin Unlimited developers

import asyncio
import time
from test_framework.util import assert_equal, assert_raises
from test_framework.test_framework import BitcoinTestFramework
from test_framework.loginit import logging
from test_framework.electrumutil import (ElectrumConnection,
    bitcoind_electrum_args, sync_electrum_height,
    wait_for_electrum_mempool)

ADDRESS_SUBSCRIBE = 'blockchain.address.subscribe'
ADDRESS_UNSUBSCRIBE = 'blockchain.address.unsubscribe'

SCRIPTHASH_SUBSCRIBE = 'blockchain.scripthash.subscribe'
SCRIPTHASH_UNSUBSCRIBE = 'blockchain.scripthash.unsubscribe'

async def address_to_address(a):
    return a

async def address_to_scripthash(address):
    cli = ElectrumConnection()
    await cli.connect()
    scripthash = await cli.call('blockchain.address.get_scripthash', address)
    cli.disconnect()
    return scripthash

class ElectrumSubscriptionsTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [bitcoind_electrum_args()]

    def run_test(self):
        n = self.nodes[0]
        n.generate(200)
        sync_electrum_height(n)


        async def async_tests():
            await self.test_subscribe_address(n)
            await self.test_subscribe_scripthash(n)
            await self.test_unsubscribe_address(n)
            await self.test_unsubscribe_scripthash(n)
            await self.test_subscribe_headers(n)
            await self.test_multiple_client_subs(n)

        loop = asyncio.get_event_loop()
        loop.run_until_complete(async_tests())

    async def test_unsubscribe_scripthash(self, n):
        return await self.test_unsubscribe(n,
                SCRIPTHASH_SUBSCRIBE, SCRIPTHASH_UNSUBSCRIBE,
                address_to_scripthash)

    async def test_unsubscribe_address(self, n):
        return await self.test_unsubscribe(n,
                ADDRESS_SUBSCRIBE, ADDRESS_UNSUBSCRIBE,
                address_to_address)

    async def test_unsubscribe(self, n, subscribe, unsubscribe, addr_converter):
        cli = ElectrumConnection()
        await cli.connect()
        addr = n.getnewaddress()
        _, queue = await cli.subscribe(subscribe, await addr_converter(addr))

        # Verify that we're receiving notifications
        n.sendtoaddress(addr, 10)
        subscription_name, _ = await asyncio.wait_for(queue.get(), timeout = 10)
        assert_equal(await addr_converter(addr), subscription_name)

        ok = await cli.call(unsubscribe, await addr_converter(addr))
        assert(ok)

        # Verify that we're no longer receiving notifications
        n.sendtoaddress(addr, 10)
        try:
            await asyncio.wait_for(queue.get(), timeout = 10)
            assert(False) # Should have timed out.
        except asyncio.TimeoutError:
            pass

        # Unsubscribing from a hash we're not subscribed to should return false
        ok = await cli.call(unsubscribe, await addr_converter(n.getnewaddress()))
        assert(not ok)


    async def test_subscribe_scripthash(self, n):
        return await self.test_subscribe(n,
                SCRIPTHASH_SUBSCRIBE, SCRIPTHASH_UNSUBSCRIBE,
                address_to_scripthash)

    async def test_subscribe_address(self, n):
        return await self.test_subscribe(n,
                ADDRESS_SUBSCRIBE, ADDRESS_UNSUBSCRIBE,
                address_to_address)

    async def test_subscribe(self, n, subscribe, unsubscribe, addr_converter):
        cli = ElectrumConnection()
        await cli.connect()

        logging.info("Testing scripthash subscription")
        addr = n.getnewaddress()
        statushash, queue = await cli.subscribe(subscribe, await addr_converter(addr))

        logging.info("Unused address should not have a statushash")
        assert_equal(None, statushash)

        logging.info("Check notification on receiving coins")
        n.sendtoaddress(addr, 10)
        subscription_name, new_statushash1 = await asyncio.wait_for(queue.get(), timeout = 10)
        assert_equal(await addr_converter(addr), subscription_name)
        assert(new_statushash1 != None and len(new_statushash1) == 64)

        logging.info("Check notification on block confirmation")
        assert(len(n.getrawtxpool()) == 1)
        n.generate(1)
        assert(len(n.getrawtxpool()) == 0)
        subscription_name, new_statushash2 = await asyncio.wait_for(queue.get(), timeout = 10)
        assert_equal(await addr_converter(addr), subscription_name)
        assert(new_statushash2 != new_statushash1)
        assert(new_statushash2 != None)

        logging.info("Check that we get notification when spending funds from address")
        n.sendtoaddress(n.getnewaddress(), n.getbalance(), "", "", True)
        subscription_name, new_statushash3 = await asyncio.wait_for(queue.get(), timeout = 10)
        assert_equal(await addr_converter(addr), subscription_name)
        assert(new_statushash3 != new_statushash2)
        assert(new_statushash3 != None)

        # Clear mempool
        n.generate(1)

    async def test_subscribe_headers(self, n):
        cli = ElectrumConnection()
        await cli.connect()
        headers = []

        logging.info("Calling subscribe should return the current best block header")
        result, queue = await cli.subscribe('blockchain.headers.subscribe')
        assert_equal(
                n.getblockheader(n.getbestblockhash(), False),
                result['hex'])

        logging.info("Now generate 10 blocks, check that these are pushed to us.")
        async def test():
            for _ in range(10):
                blockhashes = n.generate(1)
                header_hex = n.getblockheader(blockhashes.pop(), False)
                notified = await asyncio.wait_for(queue.get(), timeout = 10)
                assert_equal(header_hex, notified.pop()['hex'])

        start = time.time()
        await test()
        logging.info("Getting 10 block notifications took {} seconds".format(time.time() - start))


    async def test_multiple_client_subs(self, n):
        num_clients = 50
        clients = [ ElectrumConnection() for _ in range(0, num_clients) ]
        [ await c.connect() for c in clients ]

        queues = []
        addresses = [ n.getnewaddress() for _ in range(0, num_clients) ]

        # Send coins so the addresses, so they get a statushash
        [ n.sendtoaddress(addresses[i], 10) for i in range(0, num_clients) ]

        wait_for_electrum_mempool(n, count = num_clients)

        statushashes = []
        queues = []
        for i in range(0, num_clients):
            cli = clients[i]
            addr = addresses[i]
            scripthash = await address_to_scripthash(addr)
            statushash, queue = await cli.subscribe(SCRIPTHASH_SUBSCRIBE, scripthash)

            # should be unique
            assert(statushash is not None)
            assert(statushash not in statushashes)
            statushashes.append(statushash)
            queues.append(queue)

        # Send new coin to all, observe that all clients get a notification
        [ n.sendtoaddress(addresses[i], 10) for i in range(0, num_clients) ]

        for i in range(0, num_clients):
            q = queues[i]
            old_statushash = statushashes[i]

            scripthash, new_statushash = await asyncio.wait_for(q.get(), timeout = 10)
            assert_equal(scripthash, await address_to_scripthash(addresses[i]))
            assert(new_statushash != None)
            assert(new_statushash != old_statushash)


if __name__ == '__main__':
    ElectrumSubscriptionsTest().main()
