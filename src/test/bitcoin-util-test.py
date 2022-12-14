#!/usr/bin/python
# Copyright 2014 BitPay, Inc.
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
from __future__ import division,print_function,unicode_literals
import os
import bctest
import buildenv
# Note: the program .../qa/rpc-tests/bitcoin_util_test_data_generate.py can be used to create many of the input and output hex files used by this test
if __name__ == '__main__':
	bctest.bctester(os.environ["srcdir"] + "/test/data",
			"bitcoin-util-test.json",buildenv)
