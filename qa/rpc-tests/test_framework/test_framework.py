#!/usr/bin/env python3
# Copyright (c) 2014-2015 The Bitcoin Core developers
# Copyright (c) 2015-2017 The Bitcoin Unlimited developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Base class for RPC testing

import logging
import optparse
import os
import sys
import time       # BU added
import random     # BU added
import pdb        # BU added
import shutil
import tempfile
import traceback
from os.path import basename
import string
from sys import argv

from .util import (
    initialize_chain,
    initialize_chain_clean,
    assert_equal,
    start_node,
    start_nodes,
    connect_nodes_bi,
    sync_blocks,
    sync_mempools,
    stop_nodes,
    wait_bitcoinds,
    wait_bitcoind_exit,
    enable_coverage,
    check_json_precision,
    initialize_chain_clean,
    PortSeed,
    UtilOptions,
    COINBASE_REWARD
)


from .authproxy import JSONRPCException


class BitcoinTestFramework(object):
    drop_to_pdb = os.getenv("DROP_TO_PDB", "")
    bins = None
    setup_clean_chain = False
    num_nodes = 4
    extra_args = None

    def __init__(self):
        super().__init__()
        self.set_test_params()

    # These may be over-ridden by subclasses:
    def set_test_params(self):
        pass

    def run_test(self):
        for node in self.nodes:
            assert_equal(node.getblockcount(), 200)
            assert_equal(node.getbalance(), 25*COINBASE_REWARD)

    def add_options(self, parser):
        pass

    def setup_chain(self,bitcoinConfDict=None, wallets=None):
        """
        Sets up the blockchain for the bitcoin nodes.  It also sets up the daemon configuration.
        bitcoinConfDict:  Pass a dictionary of values you want written to bitcoin.conf.  If you have a key with multiple values, pass a list of the values as the value, for example:
        { "debug":["net","blk","thin","lck","mempool","req","bench","evict"] }
        This framework provides values for the necessary fields (like regtest=1).  But you can override these
        defaults by setting them in this dictionary.

        wallets: Pass a list of wallet filenames.  Each wallet file will be copied into the node's directory
        before starting the node.
        """
        logging.info("Initializing test directory %s Bitcoin conf: %s walletfiles: %s" % (self.options.tmpdir, str(bitcoinConfDict), wallets))
        if self.setup_clean_chain:
            initialize_chain_clean(self.options.tmpdir, self.num_nodes, bitcoinConfDict, wallets)
        else:
            # Make sure we get the right bitcoind for each node
            if self.bins is None:
                self.bins = [self.bitcoindBin] * 10
            initialize_chain(self.options.tmpdir,bitcoinConfDict, wallets, self.bins)

    def setup_nodes(self):
        return start_nodes(self.num_nodes, self.options.tmpdir, extra_args = self.extra_args)

    def setup_network(self, split = False):
        self.nodes = self.setup_nodes()

        if self.num_nodes == 1:
            self.is_network_split = False
            return

        if self.num_nodes == 2:
            if split:
                raise Exception("split option for 2 nodes NYI")

            connect_nodes_bi(self.nodes, 0, 1)
            self.is_network_split = False
            self.sync_all()
            return

        if self.num_nodes != 4:
            raise Exception("Default setup_network for %d nodes NYI" % self.num_nodes)

        # Connect the nodes as a "chain".  This allows us
        # to split the network between nodes 1 and 2 to get
        # two halves that can work on competing chains.

        # If we joined network halves, connect the nodes from the joint
        # on outward.  This ensures that chains are properly reorganised.
        if not split:
            connect_nodes_bi(self.nodes, 1, 2)
            sync_blocks(self.nodes[1:3])
            sync_mempools(self.nodes[1:3])

        connect_nodes_bi(self.nodes, 0, 1)
        connect_nodes_bi(self.nodes, 2, 3)
        self.is_network_split = split
        self.sync_all()

    def stop_node(self, i, expected_stderr='', wait=0):
        """Stop a nexad test node"""
        stop_nodes([self.nodes[i]])
        self.wait_for_node_exit(i, 60)

    def stop_nodes(self, wait=0):
        """Stop multiple nexad test nodes"""
        return stop_nodes(self.nodes)

    def start_node(self, i, extra_args=None):
        return start_node(i, self.options.tmpdir, extra_args)

    def restart_node(self, i, extra_args=None):
        """Stop and start a test node"""
        self.stop_node(i)
        self.start_node(i, extra_args)

    def wait_for_node_exit(self, i, timeout):
        wait_bitcoind_exit(i,timeout)

    def split_network(self):
        """
        Split the network of four nodes into nodes 0/1 and 2/3.
        """
        assert not self.is_network_split
        stop_nodes(self.nodes)
        wait_bitcoinds()
        self.setup_network(True)

    def sync_all(self):
        """Synchronizes blocks and mempools"""
        if self.is_network_split:
            sync_blocks(self.nodes[:2])
            sync_blocks(self.nodes[2:])
            sync_mempools(self.nodes[:2])
            sync_mempools(self.nodes[2:])
        else:
            sync_blocks(self.nodes)
            sync_mempools(self.nodes)

    def sync_blocks(self):
        """Synchronizes blocks"""
        if self.is_network_split:
            sync_blocks(self.nodes[:2])
            sync_blocks(self.nodes[2:])
        else:
            sync_blocks(self.nodes)

    def join_network(self):
        """
        Join the (previously split) network halves together.
        """
        assert self.is_network_split
        stop_nodes(self.nodes)
        wait_bitcoinds()
        self.setup_network(False)

    def main(self,argsOverride=None,bitcoinConfDict=None,wallets=None):
        """
        argsOverride: pass your own values for sys.argv in this field (or pass None) to use sys.argv
        bitcoinConfDict:  Pass a dictionary of values you want written to bitcoin.conf.  If you have a key with multiple values, pass a list of the values as the value, for example:
        { "debug":["net","blk","thin","lck","mempool","req","bench","evict"] }
        This framework provides values for the necessary fields (like regtest=1).  But you can override these
        defaults by setting them in this dictionary.

        wallets: Pass a list of wallet filenames.  Each wallet file will be copied into the node's directory
        before starting the node.
        """

        parser = optparse.OptionParser(usage="%prog [options]")
        parser.add_option("--nocleanup", dest="nocleanup", default=False, action="store_true",
                          help="Leave nexads and test.* datadir on exit or error")
        parser.add_option("--noshutdown", dest="noshutdown", default=False, action="store_true",
                          help="Don't stop nexads after the test execution")
        parser.add_option("--srcdir", dest="srcdir", default=os.path.normpath(os.path.dirname(os.path.realpath(__file__))+"/../../../src"),
                          help="Source directory containing nexad/bitcoin-cli (default: %default)")


        testname = "".join(
            filter(lambda x : x in string.ascii_lowercase, basename(argv[0])))

        default_tempdir = tempfile.mkdtemp(prefix="test_"+testname+"_")

        parser.add_option("--tmppfx", dest="tmppfx", default=None,
                          help="Directory custom prefix for data directories, if specified, overrides tmpdir")
        parser.add_option("--tmpdir", dest="tmpdir", default=default_tempdir,
                          help="Root directory for data directories.")
        parser.add_option("--tracerpc", dest="trace_rpc", default=False, action="store_true",
                          help="Print out all RPC calls as they are made")
        parser.add_option("--portseed", dest="port_seed", default=os.getpid(), type='int',
                          help="The seed to use for assigning port numbers (default: current process id)")
        parser.add_option("--coveragedir", dest="coveragedir",
                          help="Write tested RPC commands into this directory")
        # BU: added for tests using randomness (e.g. excessive.py)
        parser.add_option("--randomseed", dest="randomseed",
                          help="Set RNG seed for tests that use randomness (ignored otherwise)")
        parser.add_option("--no-ipv6-rpc-listen", dest="no_ipv6_rpc_listen", default=False, action="store_true",
                          help="Switch off listening on the IPv6 ::1 localhost RPC port. "
                          "This is meant to deal with travis which is currently not supporting IPv6 sockets.")
        parser.add_option("--electrum.exec", dest="electrumexec",
            help="Set a custom path to the electrum server executable", default=None)
        parser.add_option("--gitlab", dest="gitlab", default=False, action="store_true",
                          help="Changes root directory for gitlab artifact exporting. overrides tmpdir and tmppfx")


        self.add_options(parser)
        (self.options, self.args) = parser.parse_args(argsOverride)

        if self.options.gitlab is True:
            basedir = os.path.normpath(os.path.dirname(os.path.realpath(__file__))+"/../../qa_tests")
            if os.path.exists(basedir) == False:
                try:
                    os.mkdir(path=basedir, mode=0o700)
                except FileExistsError as _:
                    # ignore
                    pass
            self.options.tmpdir = tempfile.mkdtemp(prefix="test_"+testname+"_", dir=basedir)


        UtilOptions.no_ipv6_rpc_listen = self.options.no_ipv6_rpc_listen
        UtilOptions.electrumexec = self.options.electrumexec

        # BU: initialize RNG seed based on time if no seed specified
        if self.options.randomseed:
            self.randomseed = int(self.options.randomseed)
        else:
            self.randomseed = int(time.time())
        random.seed(self.randomseed)
        logging.info("Random seed: %s" % self.randomseed)

        if self.options.tmppfx is not None and self.options.gitlab is False:
            i = self.options.port_seed
            # find a short path that's easy to remember compared to mkdtemp
            while os.path.exists(self.options.tmppfx + os.sep + testname[0:-2] + str(i)):
                i+=1
            self.options.tmpdir = self.options.tmppfx + os.sep + testname[0:-2] + str(i)

        if self.options.trace_rpc:
            logging.basicConfig(level=logging.DEBUG, stream=sys.stdout)

        if self.options.coveragedir:
            enable_coverage(self.options.coveragedir)

        PortSeed.n = self.options.port_seed

        os.environ['PATH'] = self.options.srcdir + ":" + os.path.join(self.options.srcdir, "qt") + ":" + os.environ['PATH']
        self.bitcoindBin = os.path.join(self.options.srcdir, "nexad")

        check_json_precision()

        # By setting the environment variable BITCOIN_CONF_OVERRIDE to "key=value,key2=value2,..." you can inject bitcoin configuration into every test
        baseConf = os.environ.get("BITCOIN_CONF_OVERRIDE")
        if baseConf is None:
            baseConf= {}
        else:
            lines = baseConf.split(",")
            baseConf = {}
            for line in lines:
                (key,val) = line.split("=")
                baseConf[key.strip()] = val.strip()

        if bitcoinConfDict is None:
            bitcoinConfDict = {}

        bitcoinConfDict.update(baseConf)
        self.confDict = bitcoinConfDict
        success = False
        try:
            try:
                os.makedirs(self.options.tmpdir, exist_ok=False)
            except FileExistsError as e:
                assert (self.options.tmpdir.count(os.sep) >= 2) # sanity check that tmpdir is not the top level before I delete stuff
                for n in range(0,8): # delete the nodeN directories so their contents dont affect the new test
                    d = self.options.tmpdir + os.sep + ("node%d" % n)
                    try:
                        shutil.rmtree(d)
                    except FileNotFoundError:
                        pass

            # Not pretty but, I changed the function signature
            # of setup_chain to allow customization of the setup.
            # However derived object may still use the old format
            if self.setup_chain.__defaults__ is None:
              self.setup_chain()
            else:
              self.setup_chain(bitcoinConfDict, wallets)

            self.setup_network()
            self.run_test()
            success = True
        except JSONRPCException as e:
            logging.error("JSONRPC error: "+e.error['message'])
            typ, value, tb = sys.exc_info()
            traceback.print_tb(tb)
            if self.drop_to_pdb: pdb.post_mortem(tb)
        except AssertionError as e:
            logging.error("Assertion failed: " + str(e))
            typ, value, tb = sys.exc_info()
            traceback.print_tb(tb)
            if self.drop_to_pdb: pdb.post_mortem(tb)
        except KeyError as e:
            logging.error("key not found: "+ str(e))
            typ, value, tb = sys.exc_info()
            traceback.print_tb(tb)
            if self.drop_to_pdb: pdb.post_mortem(tb)
        except Exception as e:
            logging.error("Unexpected exception caught during testing: " + repr(e))
            typ, value, tb = sys.exc_info()
            traceback.print_tb(tb)
            if self.drop_to_pdb: pdb.post_mortem(tb)
        except KeyboardInterrupt as e:
            logging.error("Exiting after " + repr(e))

        if not self.options.noshutdown:
            logging.info("Stopping nodes")
            if hasattr(self, "nodes"):  # nodes may not exist if there's a startup error
                stop_nodes(self.nodes)
            wait_bitcoinds()
        else:
            logging.warning("Note: nexads were not stopped and may still be running")

        if not self.options.nocleanup and not self.options.noshutdown and success:
            logging.info("Cleaning up")
            shutil.rmtree(self.options.tmpdir)

        else:
            logging.info("Not cleaning up dir %s" % self.options.tmpdir)
            if os.getenv("PYTHON_DEBUG", ""):
                # Dump the end of the debug logs, to aid in debugging rare
                # travis failures.
                import glob
                filenames = glob.glob(self.options.tmpdir + "/node*/regtest/debug.log")
                MAX_LINES_TO_PRINT = 1000
                for f in filenames:
                    print("From" , f, ":")
                    from collections import deque
                    print("".join(deque(open(f), MAX_LINES_TO_PRINT)))

        if success:
            logging.info("Tests successful")
            return 0
        else:
            logging.error("Failed")
            return 1


# Test framework for doing p2p comparison testing, which sets up some nexad
# binaries:
# 1 binary: test binary
# 2 binaries: 1 test binary, 1 ref binary
# n>2 binaries: 1 test binary, n-1 ref binaries

class ComparisonTestFramework(BitcoinTestFramework):

    # Can override the num_nodes variable to indicate how many nodes to run.
    def __init__(self):
        self.num_nodes = 2

    def add_options(self, parser):
        parser.add_option("--testbinary", dest="testbinary",
                          default=os.getenv("NEXAD", "nexad"),
                          help="nexad binary to test")
        parser.add_option("--refbinary", dest="refbinary",
                          default=os.getenv("NEXAD", "nexad"),
                          help="nexad binary to use for reference nodes (if any)")

    def setup_chain(self,bitcoinConfDict=None, wallets=None):  # BU add config params
        logging.info("Initializing test directory %s" % self.options.tmpdir)
        initialize_chain_clean(self.options.tmpdir, self.num_nodes,bitcoinConfDict, wallets)

    def setup_network(self):
        self.nodes = start_nodes(
            self.num_nodes, self.options.tmpdir,
            extra_args=[['-debug', '-whitelist=127.0.0.1']] * self.num_nodes,
            binary=[self.options.testbinary] +
            [self.options.refbinary]*(self.num_nodes-1))
