// Copyright (c) 2016-2018 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TOKEN_GROUP_RPC_H
#define TOKEN_GROUP_RPC_H

#include "chainparams.h"
#include "coins.h"
#include "consensus/grouptokens.h"
#include "consensus/validation.h"
#include "pubkey.h"
#include "script/standard.h"
#include <unordered_map>

// Pass a group and a destination address (or CNoDestination) to get the balance of all outputs in the group
// or all outputs in that group and on that destination address.
CAmount GetGroupBalance(const CGroupTokenID &grpID, const CTxDestination &dest, const CWallet *wallet);

// Returns a mapping of groupID->balance
void GetAllGroupBalances(const CWallet *wallet, std::unordered_map<CGroupTokenID, CAmount> &balances);

// Token group helper functions -- not members because they use objects not available in the consensus lib
//* Initialize the group id from an address
CGroupTokenID GetGroupToken(const CTxDestination &id);
//* Initialize a group ID from a string representation
CGroupTokenID GetGroupToken(const std::string &cashAddrGrpId, const CChainParams &params = Params());

CTxDestination ControllingAddress(const CGroupTokenID &grp, txnouttype addrType);

//* Calculate a group ID based on the provided inputs.  Pass and empty script to opRetTokDesc if there is not
// going to be an OP_RETURN output in the transaction.
CGroupTokenID findGroupId(const COutPoint &input,
    CScript opRetTokDesc,
    GroupTokenIdFlags flags,
    GroupAuthorityFlags authorityFlags,
    uint64_t &nonce);

//* Group script helper function
CScript GetScriptForDestination(const CTxDestination &dest, const CGroupTokenID &group, const CAmount &amount);

//* Create and retrieve token descriptions using an OP_RETURN
std::vector<std::string> GetTokenDescription(const CScript &script);
CScript BuildTokenDescScript(const std::vector<std::vector<unsigned char> > &desc);

#endif
