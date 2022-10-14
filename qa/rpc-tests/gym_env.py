import sys

import gym

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


class GymEnv(gym.Env):
    def __init__(self):
        self.flags = (
            cashlib.ScriptFlags.STANDARD_SCRIPT_VERIFY_FLAGS
            | cashlib.ScriptFlags.SCRIPT_ENABLE_CHECKDATASIG
        )
        self.script = []
        self.actions = OPTIONS

    def step(self, action):
        sm = cashlib.ScriptMachine(flags=self.flags, tx=None, inputIdx=0)
        self.script.append(action)
        # worked = sm.begin(CScript(self.script))
        stk = sm.stack()
        try:
            sm.eval(self.script)
        except:
            pass
        stk = sm.stack()
        error = sm.error()
        done = error != cashlib.ScriptError.SCRIPT_ERR_OK or len(self.script) >= 100
        return stk, 0, done, {}

    def reset(self):
        self.script = []


if __name__ == "__main__":
    binpath = findBitcoind()
    cashlib.init(binpath + os.sep + ".libs" + os.sep + "libnexa.so")

    env = GymEnv()
    env.reset()
    done = False
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
    i = 0
    while not done:
        state, reward, done, info = env.step(s[i])
        print(prettyStk(state))
    i += 1
