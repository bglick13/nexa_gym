// Copyright (c) 2015 The Bitcoin Core developers
// Copyright (c) 2015-2020 The Bitcoin Unlimited developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chain.h"
#include "chainparams.h"
#include "pow.h"
#include "random.h"
#include "test/test_nexa.h"
#include "util.h"

#include <boost/test/unit_test.hpp>

using namespace std;

BOOST_FIXTURE_TEST_SUITE(pow_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(GetBlockProofEquivalentTime_test)
{
    SelectParams(CBaseChainParams::LEGACY_UNIT_TESTS);
    const Consensus::Params &params = Params().GetConsensus();

    std::vector<CBlockIndex> blocks(10000);
    for (int i = 0; i < 10000; i++)
    {
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].header.height = i;
        blocks[i].header.nTime = 1269211443 + i * params.nPowTargetSpacing;
        blocks[i].header.nBits = 0x207fffff; /* target 0x7fffff000... */
        blocks[i].header.SetChainWork(
            i ? blocks[i - 1].header.aChainWork() + GetBlockProof(blocks[i - 1]) : arith_uint256(0));
    }

    for (int j = 0; j < 1000; j++)
    {
        CBlockIndex *p1 = &blocks[InsecureRandRange(10000)];
        CBlockIndex *p2 = &blocks[InsecureRandRange(10000)];
        CBlockIndex *p3 = &blocks[InsecureRandRange(10000)];

        int64_t tdiff = GetBlockProofEquivalentTime(*p1, *p2, *p3, params);
        BOOST_CHECK_EQUAL(tdiff, p1->GetBlockTime() - p2->GetBlockTime());
    }
}

static CBlockIndex GetBlockIndex(CBlockIndex *pindexPrev, int64_t nTimeInterval, uint32_t nBits)
{
    CBlockIndex block;
    block.pprev = pindexPrev;
    block.header.height = pindexPrev->height() + 1;
    block.header.nTime = pindexPrev->time() + nTimeInterval;
    block.header.nBits = nBits;
    block.BuildSkip();
    block.header.SetChainWork(pindexPrev->chainWork() + GetBlockProof(block));
    return block;
}

// Removed, this was an EDA test

// Removed, this was a DAA test

// clang-format off
double TargetFromBits(const uint32_t nBits) { return (nBits & 0xff'ff'ff) * pow(256, (nBits >> 24) - 3); }
// clang-format-on
double GetASERTApproximationError(const CBlockIndex *pindexPrev,
    const uint32_t finalBits,
    const CBlockIndex *pindexAnchorBlock)
{
    const int64_t nHeightDiff = pindexPrev->height() - pindexAnchorBlock->height();
    const int64_t nTimeDiff = pindexPrev->GetBlockTime() - pindexAnchorBlock->pprev->GetBlockTime();
    const uint32_t initialBits = pindexAnchorBlock->tgtBits();

    BOOST_CHECK(nHeightDiff >= 0);
    double dInitialPow = TargetFromBits(initialBits);
    double dFinalPow = TargetFromBits(finalBits);

    double dExponent = double(nTimeDiff - (nHeightDiff + 1) * 600) / double(2 * 24 * 3600);
    double dTarget = dInitialPow * pow(2, dExponent);

    return (dFinalPow - dTarget) / dTarget;
}

BOOST_AUTO_TEST_CASE(asert_difficulty_test)
{
    SelectParams(CBaseChainParams::LEGACY_UNIT_TESTS);
    const Consensus::Params &params = Params().GetConsensus();

    std::vector<CBlockIndex> blocks(3000 + 2 * 24 * 3600);
    const arith_uint256 powLimit = UintToArith256(params.powLimit);
    arith_uint256 currentPow = powLimit >> 3;
    uint32_t initialBits = currentPow.GetCompact();
    double dMaxErr = 0.0001166792656486;

    // Genesis block, and parent of ASERT anchor block in this test case.
    blocks[0] = CBlockIndex();
    blocks[0].header.height = 0;
    blocks[0].header.nTime = 1269211443;
    // The pre-anchor block's nBits should never be used, so we set it to a nonsense value in order to
    // trigger an error if it is ever accessed
    blocks[0].header.nBits = 0x0dedbeef;

    blocks[0].header.SetChainWork(GetBlockProof(blocks[0]));

    // Block counter.
    size_t i = 1;

    // ASERT anchor block. We give this one a solvetime of 150 seconds to ensure that
    // the solvetime between the pre-anchor and the anchor blocks is actually used.
    blocks[1] = GetBlockIndex(&blocks[0], 150, initialBits);
    // The nBits for the next block should not be equal to the anchor block's nBits
    CBlockHeader blkHeaderDummy;
    uint32_t nBits = GetNextASERTWorkRequired(&blocks[i++], &blkHeaderDummy, params, &blocks[1]);
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[1])) < dMaxErr);
    BOOST_CHECK(nBits != initialBits);

    // If we add another block at 1050 seconds, we should return to the anchor block's nBits
    blocks[i] = GetBlockIndex(&blocks[i - 1], 1050, nBits);
    nBits = GetNextASERTWorkRequired(&blocks[i++], &blkHeaderDummy, params, &blocks[1]);
    BOOST_CHECK(nBits == initialBits);
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[1])) < dMaxErr);

    currentPow = arith_uint256().SetCompact(nBits);
    // Before we do anything else, check that timestamps *before* the anchor block work fine.
    // Jumping 2 days into the past will give a timestamp before the achnor, and should halve the target
    blocks[i] = GetBlockIndex(&blocks[i - 1], 600 - 172800, nBits);
    nBits = GetNextASERTWorkRequired(&blocks[i++], &blkHeaderDummy, params, &blocks[1]);
    currentPow = arith_uint256().SetCompact(nBits);
    // Because nBits truncates target, we don't end up with exactly 1/2 the target
    BOOST_CHECK(currentPow <= arith_uint256().SetCompact(initialBits) / 2);
    BOOST_CHECK(currentPow >= arith_uint256().SetCompact(initialBits - 1) / 2);
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[1])) < dMaxErr);

    // Jumping forward 2 days should return the target to the initial value
    blocks[i] = GetBlockIndex(&blocks[i - 1], 600 + 172800, nBits);
    nBits = GetNextASERTWorkRequired(&blocks[i++], &blkHeaderDummy, params, &blocks[1]);
    currentPow = arith_uint256().SetCompact(nBits);
    BOOST_CHECK(nBits == initialBits);
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[1])) < dMaxErr);

    // Pile up some blocks every 10 mins to establish some history.
    for (; i < 150; i++)
    {
        blocks[i] = GetBlockIndex(&blocks[i - 1], 600, nBits);
        BOOST_CHECK_EQUAL(blocks[i].tgtBits(), nBits);
    }

    nBits = GetNextASERTWorkRequired(&blocks[i - 1], &blkHeaderDummy, params, &blocks[1]);

    BOOST_CHECK_EQUAL(nBits, initialBits);

    // Difficulty stays the same as long as we produce a block every 10 mins.
    for (size_t j = 0; j < 10; i++, j++)
    {
        blocks[i] = GetBlockIndex(&blocks[i - 1], 600, nBits);
        BOOST_CHECK_EQUAL(GetNextASERTWorkRequired(&blocks[i], &blkHeaderDummy, params, &blocks[1]), nBits);
    }

    // If we add a two blocks whose solvetimes together add up to 1200s,
    // then the next block's target should be the same as the one before these blocks
    // (at this point, equal to initialBits).
    blocks[i] = GetBlockIndex(&blocks[i - 1], 300, nBits);
    nBits = GetNextASERTWorkRequired(&blocks[i++], &blkHeaderDummy, params, &blocks[1]);
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[1])) < dMaxErr);
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[i - 2])) < dMaxErr); // relative
    blocks[i] = GetBlockIndex(&blocks[i - 1], 900, nBits);
    nBits = GetNextASERTWorkRequired(&blocks[i++], &blkHeaderDummy, params, &blocks[1]);
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[1])) < dMaxErr); // absolute
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[i - 2])) < dMaxErr); // relative
    BOOST_CHECK_EQUAL(nBits, initialBits);
    BOOST_CHECK(nBits != blocks[i - 1].tgtBits());

    // Same in reverse - this time slower block first, followed by faster block.
    blocks[i] = GetBlockIndex(&blocks[i - 1], 900, nBits);
    nBits = GetNextASERTWorkRequired(&blocks[i++], &blkHeaderDummy, params, &blocks[1]);
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[1])) < dMaxErr); // absolute
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[i - 2])) < dMaxErr); // relative
    blocks[i] = GetBlockIndex(&blocks[i - 1], 300, nBits);
    nBits = GetNextASERTWorkRequired(&blocks[i++], &blkHeaderDummy, params, &blocks[1]);
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[1])) < dMaxErr); // absolute
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[i - 2])) < dMaxErr); // relative
    BOOST_CHECK_EQUAL(nBits, initialBits);
    BOOST_CHECK(nBits != blocks[i - 1].tgtBits());

    // Jumping forward 2 days should double the target (halve the difficulty)
    blocks[i] = GetBlockIndex(&blocks[i - 1], 600 + 2 * 24 * 3600, nBits);
    nBits = GetNextASERTWorkRequired(&blocks[i++], &blkHeaderDummy, params, &blocks[1]);
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[1])) < dMaxErr); // absolute
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[i - 2])) < dMaxErr); // relative
    currentPow = arith_uint256().SetCompact(nBits) / 2;
    BOOST_CHECK_EQUAL(currentPow.GetCompact(), initialBits);

    // Jumping backward 2 days should bring target back to where we started
    blocks[i] = GetBlockIndex(&blocks[i - 1], 600 - 2 * 24 * 3600, nBits);
    nBits = GetNextASERTWorkRequired(&blocks[i++], &blkHeaderDummy, params, &blocks[1]);
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[1])) < dMaxErr); // absolute
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[i - 2])) < dMaxErr); // relative
    BOOST_CHECK_EQUAL(nBits, initialBits);

    // Jumping backward 2 days should halve the target (double the difficulty)
    blocks[i] = GetBlockIndex(&blocks[i - 1], 600 - 2 * 24 * 3600, nBits);
    nBits = GetNextASERTWorkRequired(&blocks[i++], &blkHeaderDummy, params, &blocks[1]);
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[1])) < dMaxErr); // absolute
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[i - 2])) < dMaxErr); // relative
    currentPow = arith_uint256().SetCompact(nBits);
    // Because nBits truncates target, we don't end up with exactly 1/2 the target
    BOOST_CHECK(currentPow <= arith_uint256().SetCompact(initialBits) / 2);
    BOOST_CHECK(currentPow >= arith_uint256().SetCompact(initialBits - 1) / 2);

    // And forward again
    blocks[i] = GetBlockIndex(&blocks[i - 1], 600 + 2 * 24 * 3600, nBits);
    nBits = GetNextASERTWorkRequired(&blocks[i++], &blkHeaderDummy, params, &blocks[1]);
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[1])) < dMaxErr); // absolute
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[i - 2])) < dMaxErr); // relative
    BOOST_CHECK_EQUAL(nBits, initialBits);
    blocks[i] = GetBlockIndex(&blocks[i - 1], 600 + 2 * 24 * 3600, nBits);
    nBits = GetNextASERTWorkRequired(&blocks[i++], &blkHeaderDummy, params, &blocks[1]);
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[1])) < dMaxErr); // absolute
    BOOST_CHECK(fabs(GetASERTApproximationError(&blocks[i - 1], nBits, &blocks[i - 2])) < dMaxErr); // relative
    currentPow = arith_uint256().SetCompact(nBits) / 2;
    BOOST_CHECK_EQUAL(currentPow.GetCompact(), initialBits);

    // Iterate over the entire -2*24*3600..+2*24*3600 range to check that our integer approximation:
    //   1. Should be monotonic
    //   2. Should change target at least once every 8 seconds (worst-case: 15-bit precision on nBits)
    //   3. Should never change target by more than XXXX per 1-second step
    //   4. Never exceeds dMaxError in absolute error vs a double float calculation
    //   5. Has almost exactly the dMax and dMin errors we expect for the formula
    double dMin = 0;
    double dMax = 0;
    double dErr;
    double dRelMin = 0;
    double dRelMax = 0;
    double dRelErr;
    double dMaxStep = 0;
    uint32_t nBitsRingBuffer[8];
    double dStep = 0;
    blocks[i] = GetBlockIndex(&blocks[i - 1], -2 * 24 * 3600 - 30, nBits);
    for (size_t j = 0; j < 4 * 24 * 3600 + 660; j++)
    {
        blocks[i].header.nTime++;
        nBits = GetNextASERTWorkRequired(&blocks[i], &blkHeaderDummy, params, &blocks[1]);

        if (j > 8)
        {
            // 1: Monotonic
            BOOST_CHECK(arith_uint256().SetCompact(nBits) >= arith_uint256().SetCompact(nBitsRingBuffer[(j - 1) % 8]));
            // 2: Changes at least once every 8 seconds (worst case: nBits = 1d008000 to 1d008001)
            BOOST_CHECK(arith_uint256().SetCompact(nBits) > arith_uint256().SetCompact(nBitsRingBuffer[j % 8]));
            // 3: Check 1-sec step size
            dStep = (TargetFromBits(nBits) - TargetFromBits(nBitsRingBuffer[(j - 1) % 8])) / TargetFromBits(nBits);
            if (dStep > dMaxStep)
                dMaxStep = dStep;
            BOOST_CHECK(dStep < 0.0000314812106363); // from nBits = 1d008000 to 1d008001
        }
        nBitsRingBuffer[j % 8] = nBits;

        // 4 and 5: check error vs double precision float calculation
        dErr = GetASERTApproximationError(&blocks[i], nBits, &blocks[1]);
        dRelErr = GetASERTApproximationError(&blocks[i], nBits, &blocks[i - 1]);
        if (dErr < dMin)
            dMin = dErr;
        if (dErr > dMax)
            dMax = dErr;
        if (dRelErr < dRelMin)
            dRelMin = dRelErr;
        if (dRelErr > dRelMax)
            dRelMax = dRelErr;
        BOOST_CHECK_MESSAGE(
            fabs(dErr) < dMaxErr, strprintf("solveTime: %d\tStep size: %.8f%%\tdErr: %.8f%%\tnBits: %0x\n",
                                      int64_t(blocks[i].time()) - blocks[i - 1].time(), dStep * 100, dErr * 100, nBits));
        BOOST_CHECK_MESSAGE(fabs(dRelErr) < dMaxErr,
            strprintf("solveTime: %d\tStep size: %.8f%%\tdRelErr: %.8f%%\tnBits: %0x\n",
                                int64_t(blocks[i].time()) - blocks[i - 1].time(), dStep * 100, dRelErr * 100, nBits));
    }
    auto failMsg = strprintf(
        "Min error: %16.14f%%\tMax error: %16.14f%%\tMax step: %16.14f%%\n", dMin * 100, dMax * 100, dMaxStep * 100);
    BOOST_CHECK_MESSAGE(dMin < -0.0001013168981059 && dMin > -0.0001013168981060 && dMax > 0.0001166792656485 &&
                            dMax < 0.0001166792656486,
        failMsg);
    failMsg = strprintf("Min relError: %16.14f%%\tMax relError: %16.14f%%\n", dRelMin * 100, dRelMax * 100);
    BOOST_CHECK_MESSAGE(dRelMin < -0.0001013168981059 && dRelMin > -0.0001013168981060 &&
                            dRelMax > 0.0001166792656485 && dRelMax < 0.0001166792656486,
        failMsg);

    // Difficulty increases as long as we produce fast blocks
    for (size_t j = 0; j < 100; i++, j++)
    {
        uint32_t nextBits;
        arith_uint256 currentTarget;
        currentTarget.SetCompact(nBits);

        blocks[i] = GetBlockIndex(&blocks[i - 1], 500, nBits);
        nextBits = GetNextASERTWorkRequired(&blocks[i], &blkHeaderDummy, params, &blocks[1]);
        arith_uint256 nextTarget;
        nextTarget.SetCompact(nextBits);

        // Make sure that target is decreased
        BOOST_CHECK(nextTarget <= currentTarget);

        nBits = nextBits;
    }
}

std::string StrPrintCalcArgs(const arith_uint256 refTarget,
    const int64_t targetSpacing,
    const int64_t timeDiff,
    const int64_t heightDiff,
    const arith_uint256 expectedTarget,
    const uint32_t expectednBits)
{
    return strprintf("\n"
                     "ref=         %s\n"
                     "spacing=     %d\n"
                     "timeDiff=    %d\n"
                     "heightDiff=  %d\n"
                     "expTarget=   %s\n"
                     "exp nBits=   0x%08x\n",
        refTarget.ToString(), targetSpacing, timeDiff, heightDiff, expectedTarget.ToString(), expectednBits);
}


// Tests of the CalculateASERT function.
BOOST_AUTO_TEST_CASE(calculate_asert_test)
{
    SelectParams(CBaseChainParams::LEGACY_UNIT_TESTS);
    const Consensus::Params &params = Params().GetConsensus();
    const int64_t nHalfLife = params.nASERTHalfLife;

    const arith_uint256 powLimit = UintToArith256(params.powLimit);
    arith_uint256 initialTarget = powLimit >> 4;
    int64_t height = 0;

    // The CalculateASERT function uses the absolute ASERT formulation
    // and adds +1 to the height difference that it receives.
    // The time difference passed to it must factor in the difference
    // to the *parent* of the reference block.
    // We assume the parent is ideally spaced in time before the reference block.
    static const int64_t parent_time_diff = 600;

    // Steady
    arith_uint256 nextTarget = CalculateASERT(
        initialTarget, params.nPowTargetSpacing, parent_time_diff + 600 /* nTimeDiff */, ++height, powLimit, nHalfLife);
    BOOST_CHECK(nextTarget == initialTarget);

    // A block that arrives in half the expected time
    nextTarget = CalculateASERT(
        initialTarget, params.nPowTargetSpacing, parent_time_diff + 600 + 300, ++height, powLimit, nHalfLife);
    BOOST_CHECK(nextTarget < initialTarget);

    // A block that makes up for the shortfall of the previous one, restores the target to initial
    arith_uint256 prevTarget = nextTarget;
    nextTarget = CalculateASERT(
        initialTarget, params.nPowTargetSpacing, parent_time_diff + 600 + 300 + 900, ++height, powLimit, nHalfLife);
    BOOST_CHECK(nextTarget > prevTarget);
    BOOST_CHECK(nextTarget == initialTarget);

    // Two days ahead of schedule should double the target (halve the difficulty)
    prevTarget = nextTarget;
    nextTarget =
        CalculateASERT(prevTarget, params.nPowTargetSpacing, parent_time_diff + 288 * 1200, 288, powLimit, nHalfLife);
    BOOST_CHECK(nextTarget == prevTarget * 2);

    // Two days behind schedule should halve the target (double the difficulty)
    prevTarget = nextTarget;
    nextTarget =
        CalculateASERT(prevTarget, params.nPowTargetSpacing, parent_time_diff + 288 * 0, 288, powLimit, nHalfLife);
    BOOST_CHECK(nextTarget == prevTarget / 2);
    BOOST_CHECK(nextTarget == initialTarget);

    // Ramp up from initialTarget to PowLimit - should only take 4 doublings...
    uint32_t powLimit_nBits = powLimit.GetCompact();
    uint32_t next_nBits;
    for (size_t k = 0; k < 3; k++)
    {
        prevTarget = nextTarget;
        nextTarget = CalculateASERT(
            prevTarget, params.nPowTargetSpacing, parent_time_diff + 288 * 1200, 288, powLimit, nHalfLife);
        BOOST_CHECK(nextTarget == prevTarget * 2);
        BOOST_CHECK(nextTarget < powLimit);
        next_nBits = nextTarget.GetCompact();
        BOOST_CHECK(next_nBits != powLimit_nBits);
    }

    prevTarget = nextTarget;
    nextTarget =
        CalculateASERT(prevTarget, params.nPowTargetSpacing, parent_time_diff + 288 * 1200, 288, powLimit, nHalfLife);
    next_nBits = nextTarget.GetCompact();
    BOOST_CHECK(nextTarget == prevTarget * 2);
    BOOST_CHECK(next_nBits == powLimit_nBits);

    // Fast periods now cannot increase target beyond POW limit, even if we try to overflow nextTarget.
    // prevTarget is a uint256, so 256*2 = 512 days would overflow nextTarget unless CalculateASERT
    // correctly detects this error
    nextTarget = CalculateASERT(
        prevTarget, params.nPowTargetSpacing, parent_time_diff + 512 * 144 * 600, 0, powLimit, nHalfLife);
    next_nBits = nextTarget.GetCompact();
    BOOST_CHECK(next_nBits == powLimit_nBits);

    // We also need to watch for underflows on nextTarget. We need to withstand an extra ~446 days worth of blocks.
    // This should bring down a powLimit target to the a minimum target of 1.
    nextTarget = CalculateASERT(powLimit, params.nPowTargetSpacing, 0, 2 * (256 - 33) * 144, powLimit, nHalfLife);
    next_nBits = nextTarget.GetCompact();
    BOOST_CHECK_EQUAL(next_nBits, arith_uint256(1).GetCompact());

    // Define a structure holding parameters to pass to CalculateASERT.
    // We are going to check some expected results  against a vector of
    // possible arguments.
    struct calc_params
    {
        arith_uint256 refTarget;
        int64_t targetSpacing;
        int64_t timeDiff;
        int64_t heightDiff;
        arith_uint256 expectedTarget;
        uint32_t expectednBits;
    };

    // Define some named input argument values
    const arith_uint256 SINGLE_300_TARGET{"00000000ffb1ffffffffffffffffffffffffffffffffffffffffffffffffffff"};
    const arith_uint256 FUNNY_REF_TARGET{"000000008000000000000000000fffffffffffffffffffffffffffffffffffff"};

    // Define our expected input and output values.
    // The timeDiff entries exclude the `parent_time_diff` - this is
    // added in the call to CalculateASERT in the test loop.
    const std::vector<calc_params> calculate_args = {

        /* refTarget, targetSpacing, timeDiff, heightDiff, expectedTarget, expectednBits */

        {powLimit, 600, 0, 2 * 144, powLimit >> 1, 0x1c7fffff}, {powLimit, 600, 0, 4 * 144, powLimit >> 2, 0x1c3fffff},
        {powLimit >> 1, 600, 0, 2 * 144, powLimit >> 2, 0x1c3fffff},
        {powLimit >> 2, 600, 0, 2 * 144, powLimit >> 3, 0x1c1fffff},
        {powLimit >> 3, 600, 0, 2 * 144, powLimit >> 4, 0x1c0fffff},
        {powLimit, 600, 0, 2 * (256 - 34) * 144, 3, 0x01030000},
        {powLimit, 600, 0, 2 * (256 - 34) * 144 + 119, 3, 0x01030000},
        {powLimit, 600, 0, 2 * (256 - 34) * 144 + 120, 2, 0x01020000},
        {powLimit, 600, 0, 2 * (256 - 33) * 144 - 1, 2, 0x01020000},
        {powLimit, 600, 0, 2 * (256 - 33) * 144, 1, 0x01010000}, // 1 bit less since we do not need to shift to 0
        {powLimit, 600, 0, 2 * (256 - 32) * 144, 1, 0x01010000}, // more will not decrease below 1
        {1, 600, 0, 2 * (256 - 32) * 144, 1, 0x01010000},
        {powLimit, 600, 2 * (512 - 32) * 144, 0, powLimit, powLimit_nBits},
        {1, 600, (512 - 64) * 144 * 600, 0, powLimit, powLimit_nBits},
        {powLimit, 600, 300, 1, SINGLE_300_TARGET, 0x1d00ffb1}, // clamps to powLimit
        // confuses any attempt to detect overflow by inspecting result
        {FUNNY_REF_TARGET, 600, 600 * 2 * 33 * 144, 0, powLimit, powLimit_nBits},
        {1, 600, 600 * 2 * 256 * 144, 0, powLimit, powLimit_nBits}, // overflow to exactly 2^256
        // just under powlimit (not clamped) yet over powlimit_nbits
        {1, 600, 600 * 2 * 224 * 144 - 1, 0, arith_uint256(0xffff8) << 204, powLimit_nBits},
    };

    for (auto &v : calculate_args)
    {
        nextTarget = CalculateASERT(
            v.refTarget, v.targetSpacing, parent_time_diff + v.timeDiff, v.heightDiff, powLimit, nHalfLife);
        next_nBits = nextTarget.GetCompact();
        const auto failMsg = StrPrintCalcArgs(v.refTarget, v.targetSpacing, parent_time_diff + v.timeDiff, v.heightDiff,
                                 v.expectedTarget, v.expectednBits) +
                             strprintf("nextTarget=  %s\nnext nBits=  0x%08x\n", nextTarget.ToString(), next_nBits);
        BOOST_CHECK_MESSAGE(nextTarget == v.expectedTarget && next_nBits == v.expectednBits, failMsg);
    }
}

BOOST_AUTO_TEST_SUITE_END()
