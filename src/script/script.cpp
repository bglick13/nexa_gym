// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2015-2020 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "script/script.h"
#include "core_io.h"
#include "interpreter.h"
#include "script/sighashtype.h"

#include "consensus/grouptokens.h"
#include "tinyformat.h"
#include "utilstrencodings.h"

#include <algorithm>

extern bool IsScriptGrouped(const CScript &script, CScript::const_iterator *pcin, CGroupTokenInfo *grp);

using namespace std;

const char *GetOpName(opcodetype opcode)
{
    switch (opcode)
    {
    // push value
    case OP_0:
        return "0";
    case OP_PUSHDATA1:
        return "OP_PUSHDATA1";
    case OP_PUSHDATA2:
        return "OP_PUSHDATA2";
    case OP_PUSHDATA4:
        return "OP_PUSHDATA4";
    case OP_1NEGATE:
        return "-1";
    case OP_RESERVED:
        return "OP_RESERVED";
    case OP_1:
        return "1";
    case OP_2:
        return "2";
    case OP_3:
        return "3";
    case OP_4:
        return "4";
    case OP_5:
        return "5";
    case OP_6:
        return "6";
    case OP_7:
        return "7";
    case OP_8:
        return "8";
    case OP_9:
        return "9";
    case OP_10:
        return "10";
    case OP_11:
        return "11";
    case OP_12:
        return "12";
    case OP_13:
        return "13";
    case OP_14:
        return "14";
    case OP_15:
        return "15";
    case OP_16:
        return "16";

    // control
    case OP_NOP:
        return "OP_NOP";
    case OP_VER:
        return "OP_VER";
    case OP_IF:
        return "OP_IF";
    case OP_NOTIF:
        return "OP_NOTIF";
    case OP_VERIF:
        return "OP_VERIF";
    case OP_VERNOTIF:
        return "OP_VERNOTIF";
    case OP_ELSE:
        return "OP_ELSE";
    case OP_ENDIF:
        return "OP_ENDIF";
    case OP_VERIFY:
        return "OP_VERIFY";
    case OP_RETURN:
        return "OP_RETURN";

    // stack ops
    case OP_TOALTSTACK:
        return "OP_TOALTSTACK";
    case OP_FROMALTSTACK:
        return "OP_FROMALTSTACK";
    case OP_2DROP:
        return "OP_2DROP";
    case OP_2DUP:
        return "OP_2DUP";
    case OP_3DUP:
        return "OP_3DUP";
    case OP_2OVER:
        return "OP_2OVER";
    case OP_2ROT:
        return "OP_2ROT";
    case OP_2SWAP:
        return "OP_2SWAP";
    case OP_IFDUP:
        return "OP_IFDUP";
    case OP_DEPTH:
        return "OP_DEPTH";
    case OP_DROP:
        return "OP_DROP";
    case OP_DUP:
        return "OP_DUP";
    case OP_NIP:
        return "OP_NIP";
    case OP_OVER:
        return "OP_OVER";
    case OP_PICK:
        return "OP_PICK";
    case OP_ROLL:
        return "OP_ROLL";
    case OP_ROT:
        return "OP_ROT";
    case OP_SWAP:
        return "OP_SWAP";
    case OP_TUCK:
        return "OP_TUCK";

    // splice ops
    case OP_CAT:
        return "OP_CAT";
    case OP_SPLIT:
        return "OP_SPLIT";
    case OP_NUM2BIN:
        return "OP_NUM2BIN";
    case OP_BIN2NUM:
        return "OP_BIN2NUM";
    case OP_SIZE:
        return "OP_SIZE";

    // bit logic
    case OP_INVERT:
        return "OP_INVERT";
    case OP_AND:
        return "OP_AND";
    case OP_OR:
        return "OP_OR";
    case OP_XOR:
        return "OP_XOR";
    case OP_EQUAL:
        return "OP_EQUAL";
    case OP_EQUALVERIFY:
        return "OP_EQUALVERIFY";
    case OP_RESERVED1:
        return "OP_RESERVED1";
    case OP_RESERVED2:
        return "OP_RESERVED2";

    // numeric
    case OP_1ADD:
        return "OP_1ADD";
    case OP_1SUB:
        return "OP_1SUB";
    case OP_2MUL:
        return "OP_2MUL";
    case OP_2DIV:
        return "OP_2DIV";
    case OP_NEGATE:
        return "OP_NEGATE";
    case OP_ABS:
        return "OP_ABS";
    case OP_NOT:
        return "OP_NOT";
    case OP_0NOTEQUAL:
        return "OP_0NOTEQUAL";
    case OP_ADD:
        return "OP_ADD";
    case OP_SUB:
        return "OP_SUB";
    case OP_MUL:
        return "OP_MUL";
    case OP_DIV:
        return "OP_DIV";
    case OP_MOD:
        return "OP_MOD";
    case OP_LSHIFT:
        return "OP_LSHIFT";
    case OP_RSHIFT:
        return "OP_RSHIFT";
    case OP_BOOLAND:
        return "OP_BOOLAND";
    case OP_BOOLOR:
        return "OP_BOOLOR";
    case OP_NUMEQUAL:
        return "OP_NUMEQUAL";
    case OP_NUMEQUALVERIFY:
        return "OP_NUMEQUALVERIFY";
    case OP_NUMNOTEQUAL:
        return "OP_NUMNOTEQUAL";
    case OP_LESSTHAN:
        return "OP_LESSTHAN";
    case OP_GREATERTHAN:
        return "OP_GREATERTHAN";
    case OP_LESSTHANOREQUAL:
        return "OP_LESSTHANOREQUAL";
    case OP_GREATERTHANOREQUAL:
        return "OP_GREATERTHANOREQUAL";
    case OP_MIN:
        return "OP_MIN";
    case OP_MAX:
        return "OP_MAX";
    case OP_WITHIN:
        return "OP_WITHIN";

    // crypto
    case OP_RIPEMD160:
        return "OP_RIPEMD160";
    case OP_SHA1:
        return "OP_SHA1";
    case OP_SHA256:
        return "OP_SHA256";
    case OP_HASH160:
        return "OP_HASH160";
    case OP_HASH256:
        return "OP_HASH256";
    case OP_CODESEPARATOR:
        return "OP_CODESEPARATOR";
    case OP_CHECKSIG:
        return "OP_CHECKSIG";
    case OP_CHECKSIGVERIFY:
        return "OP_CHECKSIGVERIFY";
    case OP_CHECKMULTISIG:
        return "OP_CHECKMULTISIG";
    case OP_CHECKMULTISIGVERIFY:
        return "OP_CHECKMULTISIGVERIFY";

    // expansion
    case OP_NOP1:
        return "OP_NOP1";
    case OP_CHECKLOCKTIMEVERIFY:
        return "OP_CHECKLOCKTIMEVERIFY";
    case OP_CHECKSEQUENCEVERIFY:
        return "OP_CHECKSEQUENCEVERIFY";
    case OP_NOP4:
        return "OP_NOP4";
    case OP_NOP5:
        return "OP_NOP5";
    case OP_NOP6:
        return "OP_NOP6";
    case OP_NOP7:
        return "OP_NOP7";
    case OP_NOP8:
        return "OP_NOP8";
    case OP_NOP9:
        return "OP_NOP9";
    case OP_NOP10:
        return "OP_NOP10";

    case OP_CHECKDATASIG:
        return "OP_CHECKDATASIG";
    case OP_CHECKDATASIGVERIFY:
        return "OP_CHECKDATASIGVERIFY";
    case OP_REVERSEBYTES:
        return "OP_REVERSEBYTES";

    // gitlab.com/GeneralProtocols/research/chips/-/blob/master/CHIP-2021-02-Add-Native-Introspection-Opcodes.md (TODO:
    // link to reference.cash)
    // Transaction Introspection Opcodes: see https:
    case OP_INPUTINDEX:
        return "OP_INPUTINDEX";
        break;
    case OP_ACTIVEBYTECODE:
        return "OP_ACTIVEBYTECODE";
        break;
    case OP_TXVERSION:
        return "OP_TXVERSION";
        break;
    case OP_TXINPUTCOUNT:
        return "OP_TXINPUTCOUNT";
        break;
    case OP_TXOUTPUTCOUNT:
        return "OP_TXOUTPUTCOUNT";
        break;
    case OP_TXLOCKTIME:
        return "OP_TXLOCKTIME";
        break;
    case OP_UTXOVALUE:
        return "OP_UTXOVALUE";
        break;
    case OP_UTXOBYTECODE:
        return "OP_UTXOBYTECODE";
        break;
    case OP_OUTPOINTHASH:
        return "OP_OUTPOINTHASH";
        break;
    case OP_INPUTBYTECODE:
        return "OP_INPUTBYTECODE";
        break;
    case OP_INPUTSEQUENCENUMBER:
        return "OP_INPUTSEQUENCENUMBER";
        break;
    case OP_OUTPUTVALUE:
        return "OP_OUTPUTVALUE";
        break;
    case OP_OUTPUTBYTECODE:
        return "OP_OUTPUTBYTECODE";
        break;

    // NEXA opcodes
    case OP_PLACE:
        return "OP_PLACE";
    case OP_PUSH_TX_STATE:
        return "OP_PUSH_TX_STATE";
    case OP_BIN2BIGNUM:
        return "OP_BIN2BIGNUM";
    case OP_SETBMD:
        return "OP_SETBMD";
    case OP_EXEC:
        return "OP_EXEC";

    case OP_INVALIDOPCODE:
        return "OP_INVALIDOPCODE";

    default:
        return "OP_UNKNOWN";
    }
}

bool CheckMinimalPush(const std::vector<uint8_t> &data, opcodetype opcode)
{
    // Returns true if the passed code is legal with respect to minimal push by definition.

    // Excludes OP_1NEGATE, OP_1-16 since they are by definition minimal.
    // Any other opcodes outside of this range have nothing to do with data push so are pedantically "minimal".
    if ((OP_0 > opcode) || (opcode > OP_PUSHDATA4))
    {
        return true;
    }
    if (data.size() == 0)
    {
        // Should have used OP_0.
        return opcode == OP_0;
    }
    else if (data.size() == 1 && data[0] >= 1 && data[0] <= 16)
    {
        // Should have used OP_1 .. OP_16.
        return false;
    }
    else if (data.size() == 1 && data[0] == 0x81)
    {
        // Should have used OP_1NEGATE.
        return false;
    }
    else if (data.size() <= 75)
    {
        // Must have used a direct push (opcode indicating number of bytes pushed + those bytes).
        return opcode == data.size();
    }
    else if (data.size() <= 255)
    {
        // Must have used OP_PUSHDATA.
        return opcode == OP_PUSHDATA1;
    }
    else if (data.size() <= 65535)
    {
        // Must have used OP_PUSHDATA2.
        return opcode == OP_PUSHDATA2;
    }
    return true;
}

bool CScriptNum::IsMinimallyEncoded(const std::vector<uint8_t> &vch, size_t maxIntegerSize)
{
    if (vch.size() > maxIntegerSize)
    {
        return false;
    }
    if (vch.size() > 0)
    {
        // Check that the number is encoded with the minimum possible number
        // of bytes.
        //
        // If the most-significant-byte - excluding the sign bit - is zero
        // then we're not minimal. Note how this test also rejects the
        // negative-zero encoding, 0x80.
        if ((vch.back() & 0x7f) == 0)
        {
            // One exception: if there's more than one byte and the most
            // significant bit of the second-most-significant-byte is set it
            // would conflict with the sign bit. An example of this case is
            // +-255, which encode to 0xff00 and 0xff80 respectively.
            // (big-endian).
            if (vch.size() <= 1 || (vch[vch.size() - 2] & 0x80) == 0)
            {
                return false;
            }
        }
    }
    return true;
}

bool CScriptNum::MinimallyEncode(std::vector<uint8_t> &data)
{
    if (data.size() == 0)
    {
        return false;
    }

    // If the last byte is not 0x00 or 0x80, we are minimally encoded.
    uint8_t last = data.back();
    if (last & 0x7f)
    {
        return false;
    }

    // If the script is one byte long, then we have a zero, which encodes as an
    // empty array.
    if (data.size() == 1)
    {
        data = {};
        return true;
    }

    // If the next byte has it sign bit set, then we are minimaly encoded.
    if (data[data.size() - 2] & 0x80)
    {
        return false;
    }

    // We are not minimally encoded, we need to figure out how much to trim.
    for (size_t i = data.size() - 1; i > 0; i--)
    {
        // We found a non zero byte, time to encode.
        if (data[i - 1] != 0)
        {
            if (data[i - 1] & 0x80)
            {
                // We found a byte with it sign bit set so we need one more
                // byte.
                data[i++] = last;
            }
            else
            {
                // the sign bit is clear, we can use it.
                data[i - 1] |= last;
            }

            data.resize(i);
            return true;
        }
    }

    // If we the whole thing is zeros, then we have a zero.
    data = {};
    return true;
}

std::string CScript::GetHex() const { return HexStr(begin(), end()); }
std::string CScript::GetAsm() const { return ScriptToAsmStr(*this, true); }

unsigned int CScript::GetSigOpCount(const uint32_t flags, bool fAccurate) const
{
    unsigned int n = 0;
    const_iterator pc = begin();
    opcodetype lastOpcode = OP_INVALIDOPCODE;
    while (pc < end())
    {
        opcodetype opcode;
        if (!GetOp(pc, opcode))
            break;
        if (opcode == OP_CHECKSIG || opcode == OP_CHECKSIGVERIFY)
            n++;
        if ((flags & SCRIPT_ENABLE_CHECKDATASIG) && (opcode == OP_CHECKDATASIG || opcode == OP_CHECKDATASIGVERIFY))
            n++;
        else if (opcode == OP_CHECKMULTISIG || opcode == OP_CHECKMULTISIGVERIFY)
        {
            if (fAccurate && lastOpcode >= OP_1 && lastOpcode <= OP_16)
                n += DecodeOP_N(lastOpcode);
            else
                n += MAX_PUBKEYS_PER_MULTISIG;
        }
        lastOpcode = opcode;
    }
    return n;
}

unsigned int CScript::GetSigOpCount(const uint32_t flags, const CScript &scriptSig) const
{
    if ((flags & SCRIPT_VERIFY_P2SH) == 0 || !IsPayToScriptHash())
        return GetSigOpCount(flags, true);

    // This is a pay-to-script-hash scriptPubKey;
    // get the last item that the scriptSig
    // pushes onto the stack:
    const_iterator pc = scriptSig.begin();
    StackItem data;
    while (pc < scriptSig.end())
    {
        opcodetype opcode;
        if (!scriptSig.GetOp(pc, opcode, data))
            return 0;
        if (opcode > OP_16)
            return 0;
    }

    /// ... and return its opcount:
    CScript subscript(data);
    return subscript.GetSigOpCount(flags, true);
}

bool CScript::IsPayToScriptHash(vector<unsigned char> *hashBytes) const
{
    CScript::const_iterator pc = begin();
    unsigned int offset = &pc[0] - &begin()[0];
    // Extra-fast test for pay-to-script-hash CScripts:
    if (this->size() == offset + 23 && (*this)[offset] == OP_HASH160 && (*this)[offset + 1] == 0x14 &&
        (*this)[offset + 22] == OP_EQUAL)
    {
        if (hashBytes)
        {
            hashBytes->reserve(20);
            copy(begin() + offset + 2, begin() + offset + 22, back_inserter(*hashBytes));
        }
        return true;
    }
    return false;
}

bool CScript::IsPushOnly(const_iterator pc) const
{
    while (pc < end())
    {
        opcodetype opcode;
        if (!GetOp(pc, opcode))
            return false;
        // Note that IsPushOnly() *does* consider OP_RESERVED to be a
        // push-type opcode, however execution of OP_RESERVED fails, so
        // it's not relevant to P2SH/BIP62 as the scriptSig would fail prior to
        // the P2SH special validation code being executed.
        if (opcode > OP_16)
            return false;
    }
    return true;
}

bool CScript::IsPushOnly() const { return this->IsPushOnly(begin()); }

/**
 * Create the assembly string representation of a CScript object.
 * @param[in] script    CScript object to convert into the asm string representation.
 * @param[in] fAttemptSighashDecode    Whether to attempt to decode sighash types on data within the script that matches
 * the format
 *                                     of a signature. Only pass true for scripts you believe could contain signatures.
 * For example,
 *                                     pass false, or omit the this argument (defaults to false), for scriptPubKeys.
 */
string ScriptToAsmStr(const CScript &script, const bool fAttemptSighashDecode)
{
    string str;
    opcodetype opcode;
    vector<unsigned char> vch;
    CScript::const_iterator pc = script.begin();
    while (pc < script.end())
    {
        if (!str.empty())
        {
            str += " ";
        }
        if (!script.GetOp(pc, opcode, vch))
        {
            str += "[error]";
            return str;
        }
        if (0 <= opcode && opcode <= OP_PUSHDATA4)
        {
            if (vch.size() <= CScriptNum::MAXIMUM_ELEMENT_SIZE_64_BIT)
            {
                str += strprintf("%ld", CScriptNum(vch, false, CScriptNum::MAXIMUM_ELEMENT_SIZE_64_BIT).getint64());
            }
            else
            {
                // the IsUnspendable check makes sure not to try to decode OP_RETURN data that may match the format of a
                // signature
                if (fAttemptSighashDecode && !script.IsUnspendable())
                {
                    string strSigHashDecode;
                    // goal: only attempt to decode a defined sighash type from data that looks like a signature within
                    // a scriptSig.
                    // this won't decode correctly formatted public keys in Pubkey or Multisig scripts due to
                    // the restrictions on the pubkey formats (see IsCompressedOrUncompressedPubKey) being incongruous
                    // with the
                    // checks in CheckSignatureEncoding.
                    if (CheckSignatureEncoding(vch, SCRIPT_VERIFY_STRICTENC, nullptr))
                    {
                        const SigHashType sigHashType = GetSigHashType(vch);
                        // We are just guessing that this is a signature so the sighashtype should also be valid
                        // if it is
                        if (!sigHashType.isInvalid())
                        {
                            strSigHashDecode = "[" + sigHashType.ToString() + "]";
                            RemoveSigHashType(vch); // remove the sighash type byte. it will be replaced by the decode.
                        }
                    }
                    str += HexStr(vch) + strSigHashDecode;
                }
                else
                {
                    str += HexStr(vch);
                }
            }
        }
        else
        {
            str += GetOpName(opcode);
        }
    }
    return str;
}
