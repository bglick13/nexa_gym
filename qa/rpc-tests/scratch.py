from re import L
import test_framework.loginit
import time
import sys
import copy
import io

if sys.version_info[0] < 3:
    raise "Use Python 3"
import logging

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *
import test_framework.cashlib as cashlib
from test_framework.nodemessages import *
from test_framework.script import *


def prettyStk(t):
    ret = ""
    if t[0] == cashlib.StackItemType.BYTES:
        return "d_" + hexlify(t[1]).decode()
    if t[0] == cashlib.StackItemType.BIGNUM:
        return "n_" + hexlify(t[1]).decode()
    else:
        return "u_" + hexlify(t[1]).decode()


class StepEvalEnv(BitcoinTestFramework):
    def __init__(self):
        super().__init__()
        self.flags = (
            cashlib.ScriptFlags.STANDARD_SCRIPT_VERIFY_FLAGS
            | cashlib.ScriptFlags.SCRIPT_ENABLE_CHECKDATASIG
        )
        self.script = []
        self.i = 0

    def reset(self):
        self.script = []
        self.i = 0

    def setup_chain(self, bitcoinConfDict=None, wallets=None):
        cashlib.loadCashLibOrExit(self.options.srcdir)
        print("Initializing test directory " + self.options.tmpdir)
        initialize_chain(self.options.tmpdir, bitcoinConfDict, wallets)

    def setup_network(self, split=False):
        self.nodes = start_nodes(2, self.options.tmpdir)
        connect_nodes_bi(self.nodes, 0, 1)
        self.is_network_split = False
        self.sync_all()

    def run_test(self):
        return True

    def step(self, action):
        self.script.append(action)
        count = 0
        pos = 0
        sm = cashlib.ScriptMachine(flags=flags, tx=None, inputIdx=0)
        worked = sm.begin(CScript(self.script[: self.i + 1]))
        states = []
        actions = []
        try:
            while 1:

                stk = sm.stack()
                rest = CScript(sm.script[pos:])
                textrest = []
                for (opcode, data, sop_idx) in rest.raw_iter():
                    if not data:
                        try:
                            textrest.append(OPCODE_NAMES[opcode])
                        except KeyError as e:
                            textrest.append("OP_x%X" % opcode)
                    else:
                        textrest.append("DATA(%s)" % hexlify(data))
                pos = sm.step()
                states.append([prettyStk(x) for x in stk])
                actions.append(textrest[0])
        except cashlib.Error as e:
            self.i += 1

            if str(e) == "stepped beyond end of script":
                pass
                # return (cashlib.ScriptError.SCRIPT_ERR_OK, pos)
            else:
                (err, pos) = sm.error()
                print("Error: %d (%s): %s" % (err, err.name, str(e)))
                return (err, pos)
        done = (
            sm.error()[0] != cashlib.ScriptError.SCRIPT_ERR_OK
            or len(self.script) >= 100
        )
        state = sm.stack()
        if state:
            state = prettyStk(state[0])
        return (state, 0, done, {})


def stepEval(script):
    flags = (
        cashlib.ScriptFlags.STANDARD_SCRIPT_VERIFY_FLAGS
        | cashlib.ScriptFlags.SCRIPT_ENABLE_CHECKDATASIG
    )
    sm = cashlib.ScriptMachine(flags=flags, tx=None, inputIdx=0)
    # worked = sm.begin(CScript(script[0]))
    i = 0

    while i < len(script):
        count = 0
        pos = 0
        sm = cashlib.ScriptMachine(flags=flags, tx=None, inputIdx=0)
        worked = sm.begin(CScript(script[: i + 1]))
        states = []
        actions = []
        try:
            while 1:

                stk = sm.stack()
                print("step %d" % count)
                rest = CScript(sm.script[pos:])
                textrest = []
                for (opcode, data, sop_idx) in rest.raw_iter():
                    if not data:
                        try:
                            textrest.append(OPCODE_NAMES[opcode])
                        except KeyError as e:
                            textrest.append("OP_x%X" % opcode)
                    else:
                        textrest.append("DATA(%s)" % hexlify(data))
                print("  stack: ", [prettyStk(x) for x in stk])
                print("  script: ", textrest)
                count += 1
                pos = sm.step()
                states.append([prettyStk(x) for x in stk])
                actions.append(textrest[0])

        except cashlib.Error as e:
            i += 1

            if str(e) == "stepped beyond end of script":
                pass
                # return (cashlib.ScriptError.SCRIPT_ERR_OK, pos)
            else:
                (err, pos) = sm.error()
                print("Error: %d (%s): %s" % (err, err.name, str(e)))
                return (err, pos)
    print(f"states: {states}")
    print(f"actions: {actions}")
    return (cashlib.ScriptError.SCRIPT_ERR_OK, pos)


PUB_KEY_1 = unhexlify(
    "30819e300d06092a864886f70d010101050003818c003081880281807c5c3be0a4fb51673d4027ccd2ae16d6faa5899de178d4c1d005685bf343fb4a8f8a241e8d327ada64d66652a40ecbfadf70dffe4a20e56661372c99f490a335adced6d0844c408ec25d94b6b768e1d8016e6c652f54e6b72f020b61e2e817676aee7d24e04e94880199f8763a99b025214e2804613e8cc621cff18524d428730203010001"
)
PUB_KEY_2 = unhexlify(
    "30819e300d06092a864886f70d010101050003818c0030818802818061df0b04d6f0c7bd379454c607380a83d1538f19366e0bf81fb0b884a20fdbf1dcfbb3741b3b6480d55c345a1055829aadae31e58cf9851b9aeb90b8344d984becb7ad129d24bdf32bcc9544d814b396d7014fb0a2066b019e2f3af929f24c20fc8ed313f96c165ca0dd5d2122faa49f0b80df238c3f11018f5bf112f5b611cb0203010001"
)
SMALL_NUM_1 = 0
SMALL_NUM_2 = 1
SMALL_NUM_3 = 10
BIG_NUM_1 = unhexlify("f334a8c048898e4de6b8ca6359533d7fb7c12df3619e22238400")
BIG_NUM_2 = unhexlify("ff64a8c048898e4de6b8ca6359533d7fb7c12df3619e22238400")

OPTIONS = VALID_OPCODES | {
    PUB_KEY_1,
    PUB_KEY_2,
    SMALL_NUM_1,
    SMALL_NUM_2,
    SMALL_NUM_3,
    BIG_NUM_1,
    BIG_NUM_2,
}
if __name__ == "__main__":
    txbin = unhexlify(
        "0100000003ee6cfb264416c5eb6ae631915520f18529aadec50342ac8d30b01c719529f46200000000920047304402203de0797e489002423332d44e7568ff19f66bebcebaa39c35fe654432c567cc43022064d97a58a99259348e6f348ba98bb1744c1d7e9700faa9a155115a5a25444b8341004752210371f9bb1024de3b2da052ec3c238b1917d3f3eca7c7f75798d096b7ed05dc371221028d7ef7f437223338528f7f0c042c95e1d717c0613b97078601ed3019865288eb52aeffffffffd3db8d2204c5ed5f6741a953016505d16b775fc69d9f353f135b857b1d9b0381010000009300483045022100da712b8ed1ca2a7b78f777c8872bbe314c9bbdad2e44fd87f60c8569ffdac3a70220013878512919d6b27a71cd7ed3be5b035b21da778c8afa2ca337c527b07b43b741004752210371f9bb1024de3b2da052ec3c238b1917d3f3eca7c7f75798d096b7ed05dc371221028d7ef7f437223338528f7f0c042c95e1d717c0613b97078601ed3019865288eb52aeffffffffb244ad0b4ff3ffd25661c85ace8e9af79307d89d19992a604434f984ea820eac000000009200473044022025738b81d3e92d613a33ee20bd608ceb8c949bc8cc48957036b7757c96931acf022010003ed448664b4abc23fb8106d2d00030c7d286cf6982703e8e3b72a82efe3c41004752210371f9bb1024de3b2da052ec3c238b1917d3f3eca7c7f75798d096b7ed05dc371221028d7ef7f437223338528f7f0c042c95e1d717c0613b97078601ed3019865288eb52aeffffffff0380969800000000001976a914fa9c06f766135ba96654def7f4e315a0ec4adc2d88ac4ac40000000000001976a914c1b3a40e8ab22b08586e47c1ceb287def9ad23d188ac70b55c050000000017a9141c10a9aa9c416d313f6379ee0829a7df6a6f8a598700000000"
    )
    # tx2 = CTransaction()
    # tx2.vin.append(CTxIn(COutPoint().fromIdemAndIdx(txidem, 0), amt, b"", 0xFFFFFFFF))
    # tx2.vout.append(CTxOut(amt, CScript([OP_1])))
    # sig2 = cashlib.signTxInput(tx2, 0, amt, output, destPrivKey)
    # tx2.vin[0].scriptSig = cashlib.spendscript(sig2, destPubKey)
    binpath = findBitcoind()
    cashlib.init(binpath + os.sep + ".libs" + os.sep + "libnexa.so")
    flags = (
        cashlib.ScriptFlags.STANDARD_SCRIPT_VERIFY_FLAGS
        | cashlib.ScriptFlags.SCRIPT_ENABLE_CHECKDATASIG
    )
    # sm = cashlib.ScriptMachine(flags=flags, tx=None, inputIdx=0)
    s = [
        unhexlify("0001"),
        OP_SETBMD,
        BIG_NUM_1,
        BIG_NUM_2,
        OP_BIN2BIGNUM,
        OP_SWAP,
        OP_BIN2BIGNUM,
        OP_ADD,
    ]
    # stepEval(s)
    i = 0
    env = StepEvalEnv()
    options = ["--noshutdown"]
    env.main(argsOverride=options)
    done = False
    try:
        while not done:
            state, reward, done, info = env.step(s[i])
            print(f"i: {i}\nState: {state}")
            print([prettyStk(s)] for s in state)
            i += 1
    except:
        pass
    finally:
        print("Stopping nodes)")
        if hasattr(env, "nodes"):
            stop_nodes(env.nodes)
        wait_bitcoinds()
        print("Cleaning up")
        shutil.rmtree(env.options.tmpdir)

    # if not self.options.noshutdown:
    #         logging.info("Stopping nodes")
    #         if hasattr(self, "nodes"):  # nodes may not exist if there's a startup error
    #             stop_nodes(self.nodes)
    #         wait_bitcoinds()
    #     else:
    #         logging.warning("Note: nexads were not stopped and may still be running")

    #     if not self.options.nocleanup and not self.options.noshutdown and success:
    #         logging.info("Cleaning up")
    #         shutil.rmtree(self.options.tmpdir)

    #     else:
    #         logging.info("Not cleaning up dir %s" % self.options.tmpdir)
    #         if os.getenv("PYTHON_DEBUG", ""):
    #             # Dump the end of the debug logs, to aid in debugging rare
    #             # travis failures.
    #             import glob
    #             filenames = glob.glob(self.options.tmpdir + "/node*/regtest/debug.log")
    #             MAX_LINES_TO_PRINT = 1000
    #             for f in filenames:
    #                 print("From" , f, ":")
    #                 from collections import deque
    #                 print("".join(deque(open(f), MAX_LINES_TO_PRINT)))
