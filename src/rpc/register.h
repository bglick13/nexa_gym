// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2015-2020 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef NEXA_RPC_REGISTER_H
#define NEXA_RPC_REGISTER_H

/** These are in one header file to avoid creating tons of single-function
 * headers for everything under src/rpc/ */
class CRPCTable;

/** Register block chain RPC commands */
void RegisterBlockchainRPCCommands(CRPCTable &tableRPC);
/** Register P2P networking RPC commands */
void RegisterNetRPCCommands(CRPCTable &tableRPC);
/** Register miscellaneous RPC commands */
void RegisterMiscRPCCommands(CRPCTable &tableRPC);
/** Register mining RPC commands */
void RegisterMiningRPCCommands(CRPCTable &tableRPC);
/** Register raw transaction RPC commands */
void RegisterRawTransactionRPCCommands(CRPCTable &tableRPC);
/** Register Bitcoin Unlimited's RPC commands */
void RegisterUnlimitedRPCCommands(CRPCTable &tableRPC);
/** Register Electrum RPC commands */
void RegisterElectrumRPC(CRPCTable &tableRPC);

void RegisterNexaRPCCommands(CRPCTable &table);

/** Register CAPD RPC commands */
void RegisterCapdRPCCommands(CRPCTable &table);

static inline void RegisterAllCoreRPCCommands(CRPCTable &tableRPC)
{
    RegisterBlockchainRPCCommands(tableRPC);
    RegisterNetRPCCommands(tableRPC);
    RegisterMiscRPCCommands(tableRPC);
    RegisterMiningRPCCommands(tableRPC);
    RegisterRawTransactionRPCCommands(tableRPC);
    RegisterUnlimitedRPCCommands(tableRPC);
    RegisterElectrumRPC(tableRPC);
    RegisterNexaRPCCommands(tableRPC);
    RegisterCapdRPCCommands(tableRPC);
}

#endif // NEXA_RPC_REGISTER_H
