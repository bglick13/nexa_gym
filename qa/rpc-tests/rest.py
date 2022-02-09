#!/usr/bin/env python3
# Copyright (c) 2014-2015 The Bitcoin Core developers
# Copyright (c) 2015-2017 The Bitcoin Unlimited developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
import test_framework.loginit
#
# Test REST interface
#


from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *
from test_framework.nodemessages import *
from struct import *
from io import BytesIO
from codecs import encode

import http.client
import urllib.parse

def deser_uint256(f):
    r = 0
    for i in range(8):
        t = unpack(b"<I", f.read(4))[0]
        r += t << (i * 32)
    return r

#allows simple http get calls
def http_get_call(host, port, path, response_object = 0):
    conn = http.client.HTTPConnection(host, port)
    conn.request('GET', path)

    if response_object:
        return conn.getresponse()

    return conn.getresponse().read().decode('utf-8')

#allows simple http post calls with a request body
def http_post_call(host, port, path, requestdata = '', response_object = 0):
    conn = http.client.HTTPConnection(host, port)
    conn.request('POST', path, requestdata)

    if response_object:
        return conn.getresponse()

    return conn.getresponse().read()

class RESTTest (BitcoinTestFramework):
    FORMAT_SEPARATOR = "."

    def setup_chain(self):
        print("Initializing test directory "+self.options.tmpdir)
        initialize_chain_clean(self.options.tmpdir, 3)

    def setup_network(self, split=False):
        self.nodes = start_nodes(3, self.options.tmpdir)
        connect_nodes_full(self.nodes)
        self.is_network_split=False
        self.sync_all()

    def run_test(self):
        url = urllib.parse.urlparse(self.nodes[0].url)
        print("Mining blocks...")

        self.nodes[0].generate(1)
        self.sync_all()
        self.nodes[2].generate(100)
        self.sync_all()

        assert_equal(self.nodes[0].getbalance(), COINBASE_REWARD)

        txid = self.nodes[0].sendtoaddress(self.nodes[1].getnewaddress(), 100000)
        self.sync_all()
        self.nodes[2].generate(1)
        self.sync_all()
        bb_hash = self.nodes[0].getbestblockhash()

        assert_equal(self.nodes[1].getbalance(), Decimal("100000")) #balance now should be 0.1 on node 1

        # load the latest 0.1 tx over the REST API
        json_string = http_get_call(url.hostname, url.port, '/rest/tx/'+txid+self.FORMAT_SEPARATOR+"json")
        json_obj = json.loads(json_string)
        outpoint = json_obj['vin'][0]['outpoint'] # get the vin to later check for utxo (should be spent by then)
        # get n of 0.1 outpoint
        n = 0
        for vout in json_obj['vout']:
            if vout['value'] == 100000:
                n = vout['n']


        ######################################
        # GETUTXOS: query a unspent outpoint #
        ######################################
        json_request = '/checkmempool/'+txid+'-'+str(n)
        json_string = http_get_call(url.hostname, url.port, '/rest/getutxos'+json_request+self.FORMAT_SEPARATOR+'json')
        json_obj = json.loads(json_string)

        #check chainTip response
        assert_equal(json_obj['chaintipHash'], bb_hash)

        #make sure there is one utxo
        assert_equal(len(json_obj['utxos']), 1)
        assert_equal(json_obj['utxos'][0]['value'], 100000)


        ################################################
        # GETUTXOS: now query a already spent outpoint #
        ################################################
        json_request = '/checkmempool/'+outpoint
        json_string = http_get_call(url.hostname, url.port, '/rest/getutxos'+json_request+self.FORMAT_SEPARATOR+'json')
        json_obj = json.loads(json_string)

        #check chainTip response
        assert_equal(json_obj['chaintipHash'], bb_hash)

        #make sure there is no utox in the response because this oupoint has been spent
        assert_equal(len(json_obj['utxos']), 0)

        #check bitmap
        assert_equal(json_obj['bitmap'], "0")


        ##################################################
        # GETUTXOS: now check both with the same request #
        ##################################################
        json_request = '/checkmempool/'+txid+'-'+str(n)+'/'+outpoint
        json_string = http_get_call(url.hostname, url.port, '/rest/getutxos'+json_request+self.FORMAT_SEPARATOR+'json')
        json_obj = json.loads(json_string)
        assert_equal(len(json_obj['utxos']), 1)
        assert_equal(json_obj['bitmap'], "10")

        #test binary response
        bb_hash = self.nodes[0].getbestblockhash()

        binaryRequest = b'\x01\x02'
        binaryRequest += hex_str_to_bytes(txid)
        binaryRequest += pack("i", n)
        binaryRequest += hex_str_to_bytes(outpoint)
        binaryRequest += pack("i", 0)

        bin_response = http_post_call(url.hostname, url.port, '/rest/getutxos'+self.FORMAT_SEPARATOR+'bin', binaryRequest)
        output = BytesIO()
        output.write(bin_response)
        output.seek(0)
        chainHeight = unpack("q", output.read(8))[0]
        hashFromBinResponse = hex(deser_uint256(output))[2:].zfill(64)

        assert_equal(bb_hash, hashFromBinResponse) #check if getutxo's chaintip during calculation was fine
        assert_equal(chainHeight, 102) #chain height must be 102


        ############################
        # GETUTXOS: mempool checks #
        ############################

        # do a tx and don't sync
        txid = self.nodes[0].sendtoaddress(self.nodes[1].getnewaddress(), 100000)
        json_string = http_get_call(url.hostname, url.port, '/rest/tx/'+txid+self.FORMAT_SEPARATOR+"json")
        json_obj = json.loads(json_string)
        outpoint = json_obj['vin'][0]['outpoint'] # get the vin to later check for utxo (should be spent by then)
        # get n of 0.1 outpoint
        n = 0
        for vout in json_obj['vout']:
            if vout['value'] == 100000:
                n = vout['n']

        json_request = '/'+txid+'-'+str(n)
        json_string = http_get_call(url.hostname, url.port, '/rest/getutxos'+json_request+self.FORMAT_SEPARATOR+'json')
        json_obj = json.loads(json_string)
        assert_equal(len(json_obj['utxos']), 0) #there should be a outpoint because it has just added to the mempool

        json_request = '/checkmempool/'+txid+'-'+str(n)
        json_string = http_get_call(url.hostname, url.port, '/rest/getutxos'+json_request+self.FORMAT_SEPARATOR+'json')
        json_obj = json.loads(json_string)
        assert_equal(len(json_obj['utxos']), 1) #there should be a outpoint because it has just added to the mempool

        #do some invalid requests
        json_request = '{"checkmempool'
        response = http_post_call(url.hostname, url.port, '/rest/getutxos'+self.FORMAT_SEPARATOR+'json', json_request, True)
        assert_equal(response.status, 500) #must be a 500 because we send a invalid json request

        json_request = '{"checkmempool'
        response = http_post_call(url.hostname, url.port, '/rest/getutxos'+self.FORMAT_SEPARATOR+'bin', json_request, True)
        assert_equal(response.status, 500) #must be a 500 because we send a invalid bin request

        response = http_post_call(url.hostname, url.port, '/rest/getutxos/checkmempool'+self.FORMAT_SEPARATOR+'bin', '', True)
        assert_equal(response.status, 500) #must be a 500 because we send a invalid bin request

        #test limits
        json_request = '/checkmempool/'
        for x in range(0, 20):
            json_request += txid+'-'+str(n)+'/'
        json_request = json_request.rstrip("/")
        response = http_post_call(url.hostname, url.port, '/rest/getutxos'+json_request+self.FORMAT_SEPARATOR+'json', '', True)
        assert_equal(response.status, 500) #must be a 500 because we exceeding the limits

        json_request = '/checkmempool/'
        for x in range(0, 15):
            json_request += txid+'-'+str(n)+'/'
        json_request = json_request.rstrip("/")
        response = http_post_call(url.hostname, url.port, '/rest/getutxos'+json_request+self.FORMAT_SEPARATOR+'json', '', True)
        assert_equal(response.status, 200) #must be a 500 because we exceeding the limits

        self.nodes[0].generate(1) #generate block to not affect upcoming tests
        self.sync_all()

        ################
        # /rest/block/ #
        ################

        MaxHeaderSize = 512  # May need to change as header variable length fields change
        MinHeaderSize = 180

        # check binary format
        response = http_get_call(url.hostname, url.port, '/rest/block/'+bb_hash+self.FORMAT_SEPARATOR+"bin", True)
        assert_equal(response.status, 200)
        assert_greater_than(int(response.getheader('content-length')), 80)
        response_str = response.read()

        # compare with block header
        response_header = http_get_call(url.hostname, url.port, '/rest/headers/1/'+bb_hash+self.FORMAT_SEPARATOR+"bin", True)
        assert_equal(response_header.status, 200)
        assert int(response_header.getheader('content-length')) <= MaxHeaderSize
        response_header_str = response_header.read()
        assert_equal(response_str[0:len(response_header_str)], response_header_str)

        # check block hex format
        response_hex = http_get_call(url.hostname, url.port, '/rest/block/'+bb_hash+self.FORMAT_SEPARATOR+"hex", True)
        assert_equal(response_hex.status, 200)
        content_len = int(response_hex.getheader('content-length'))
        response_hex_str = response_hex.read().decode()
        response_obj = CBlock(response_hex_str)  # Make sure we can deserialize what we got into a block object
        assert content_len <= MaxHeaderSize*2
        assert_equal(encode(response_str, "hex_codec").decode()[0:160], response_hex_str[0:160])

        # compare with hex block header (serialization of a block is its header serialization concatenated with tx array)
        response_header_hex = http_get_call(url.hostname, url.port, '/rest/headers/1/'+bb_hash+self.FORMAT_SEPARATOR+"hex", True)
        assert_equal(response_header_hex.status, 200)
        assert_greater_than(int(response_header_hex.getheader('content-length')), MinHeaderSize*2)
        response_header_hex_str = response_header_hex.read().decode().strip()
        assert_equal(response_hex_str[0:len(response_header_hex_str)], response_header_hex_str)
        assert_equal(encode(response_header_str, "hex_codec").decode()[0:len(response_header_hex_str)], response_header_hex_str)

        # check json format
        block_json_string = http_get_call(url.hostname, url.port, '/rest/block/'+bb_hash+self.FORMAT_SEPARATOR+'json')
        block_json_obj = json.loads(block_json_string)
        assert_equal(block_json_obj['hash'], bb_hash)

        # compare with json block header
        response_header_json = http_get_call(url.hostname, url.port, '/rest/headers/1/'+bb_hash+self.FORMAT_SEPARATOR+"json", True)
        assert_equal(response_header_json.status, 200)
        response_header_json_str = response_header_json.read().decode('utf-8')
        json_obj = json.loads(response_header_json_str, parse_float=Decimal)
        assert_equal(len(json_obj), 1) #ensure that there is one header in the json response
        assert_equal(json_obj[0]['hash'], bb_hash) #request/response hash should be the same

        #compare with normal RPC block response
        rpc_block_json = self.nodes[0].getblock(bb_hash)
        assert_equal(json_obj[0]['hash'],               rpc_block_json['hash'])
        assert_equal(json_obj[0]['confirmations'],      rpc_block_json['confirmations'])
        assert_equal(json_obj[0]['height'],             rpc_block_json['height'])
        assert_equal(json_obj[0]['merkleroot'],         rpc_block_json['merkleroot'])
        assert_equal(json_obj[0]['time'],               rpc_block_json['time'])
        assert_equal(json_obj[0]['nonce'],              rpc_block_json['nonce'])
        assert_equal(json_obj[0]['bits'],               rpc_block_json['bits'])
        assert_equal(json_obj[0]['difficulty'],         rpc_block_json['difficulty'])
        assert_equal(json_obj[0]['chainwork'],          rpc_block_json['chainwork'])
        assert_equal(json_obj[0]['previousblockhash'],  rpc_block_json['previousblockhash'])
        assert_equal(json_obj[0]['size'],               rpc_block_json['size'])
        assert_equal(json_obj[0]['utxoCommitment'],     rpc_block_json['utxoCommitment'])
        assert_equal(json_obj[0]['minerData'],          rpc_block_json['minerData'])
        assert_equal(json_obj[0]['ancestorhash'],       rpc_block_json['ancestorhash'])

        #see if we can get 5 headers in one response
        self.nodes[1].generate(5)
        self.sync_all()
        response_header_json = http_get_call(url.hostname, url.port, '/rest/headers/5/'+bb_hash+self.FORMAT_SEPARATOR+"json", True)
        assert_equal(response_header_json.status, 200)
        response_header_json_str = response_header_json.read().decode('utf-8')
        json_obj = json.loads(response_header_json_str)
        assert_equal(len(json_obj), 5) #now we should have 5 header objects

        # do tx test
        tx_hash = block_json_obj['tx'][0]['txidem']
        json_string = http_get_call(url.hostname, url.port, '/rest/tx/'+tx_hash+self.FORMAT_SEPARATOR+"json")
        json_obj = json.loads(json_string)
        assert_equal(json_obj['txidem'], tx_hash)

        # check hex format response
        hex_string = http_get_call(url.hostname, url.port, '/rest/tx/'+tx_hash+self.FORMAT_SEPARATOR+"hex", True)
        assert_equal(hex_string.status, 200)
        assert_greater_than(int(response.getheader('content-length')), 10)


        # check block tx details
        # let's make 3 tx and mine them on node 1
        txs = []
        txs.append(self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), 3000000))
        txs.append(self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), 3000000))
        txs.append(self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), 3000000))
        self.sync_all()

        # check that there are exactly 3 transactions in the TX memory pool before generating the block
        json_string = http_get_call(url.hostname, url.port, '/rest/mempool/info'+self.FORMAT_SEPARATOR+'json')
        json_obj = json.loads(json_string)
        assert_equal(json_obj['size'], 3)
        # the size of the memory pool should be greater than 3x ~100 bytes
        assert_greater_than(json_obj['bytes'], 300)

        # check that there are our submitted transactions in the TX memory pool
        json_string = http_get_call(url.hostname, url.port, '/rest/mempool/contents'+self.FORMAT_SEPARATOR+'json')
        json_obj = json.loads(json_string)
        for i, tx in enumerate(txs):
            assert_equal(tx in json_obj, True)
            assert_equal(json_obj[tx]['spentby'], txs[i+1:i+2])
            assert_equal(json_obj[tx]['depends'], txs[i-1:i])

        # now mine the transactions
        newblockhash = self.nodes[1].generate(1)
        self.sync_all()

        #check if the 3 tx show up in the new block
        json_string = http_get_call(url.hostname, url.port, '/rest/block/'+newblockhash[0]+self.FORMAT_SEPARATOR+'json')
        json_obj = json.loads(json_string)
        for tx in json_obj['tx']:
            if not len(tx['vin']) == 0: # exclude coinbase
                assert tx['txidem'] in txs

        #check the same but without tx details
        json_string = http_get_call(url.hostname, url.port, '/rest/block/notxdetails/'+newblockhash[0]+self.FORMAT_SEPARATOR+'json')
        json_obj = json.loads(json_string)
        for tx in txs:
            assert_equal(tx in json_obj['txidem'], True)

        #test rest bestblock
        bb_hash = self.nodes[0].getbestblockhash()

        json_string = http_get_call(url.hostname, url.port, '/rest/chaininfo.json')
        json_obj = json.loads(json_string)
        assert_equal(json_obj['bestblockhash'], bb_hash)

if __name__ == '__main__':
    RESTTest ().main ()

def Test():
    t = RESTTest()
    t.drop_to_pdb = True
    bitcoinConf = {
        "debug": ["graphene", "blk", "mempool", "net", "req"],
    }
    flags = standardFlags()
    t.main(flags, bitcoinConf, None)
