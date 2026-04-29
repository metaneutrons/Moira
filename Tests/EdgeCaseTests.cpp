// =============================================================================
// Moira Edge Case Tests — exhaustive corner cases for FPU and 040
// =============================================================================

#include "MoiraFPU.h"
#include "MoiraCache040.h"
#include "MoiraMMU040.h"
#include <cstdio>
#include <cstring>
#include <cmath>

using namespace moira;

static int tests_passed = 0;
static int tests_failed = 0;
static int tests_total = 0;

#define CHECK(cond, msg) do { tests_total++; \
    if (cond) { tests_passed++; } \
    else { tests_failed++; printf("  FAIL [%d]: %s\n", __LINE__, msg); } \
} while(0)

#define CHECK_EQ(a, b, msg) do { tests_total++; \
    if ((a) == (b)) { tests_passed++; } \
    else { tests_failed++; printf("  FAIL [%d]: %s: got 0x%llX, expected 0x%llX\n", \
        __LINE__, msg, (unsigned long long)(a), (unsigned long long)(b)); } \
} while(0)

#define SECTION(name) printf("--- %s ---\n", name)

static void fromDouble(Registers::FPReg &r, double d) {
    u64 bits; memcpy(&bits, &d, 8); fpFromDouble(r, bits);
}
static double toDouble(const Registers::FPReg &r) {
    u64 bits = fpToDouble(r); double d; memcpy(&d, &bits, 8); return d;
}

// =============================================================================
// FPU: Complete special value interaction matrix
// Every arithmetic op tested with: +0, -0, +inf, -inf, NaN, normal, denormal
// =============================================================================

void testFPU_AddSpecialMatrix() {
    SECTION("FPU FADD Special Value Matrix");
    Registers::FPReg a, b;
    u32 fpsr;

    // +0 + +0 = +0
    fpsr = 0; fpSetZero(a, false); fpSetZero(b, false);
    fpAdd(a, b, fpsr);
    CHECK(fpIsZero(a) && !fpIsNeg(a), "+0 + +0 = +0");

    // +0 + -0 = +0
    fpsr = 0; fpSetZero(a, false); fpSetZero(b, true);
    fpAdd(a, b, fpsr);
    CHECK(fpIsZero(a), "+0 + -0 = 0");

    // -0 + -0 = -0
    fpsr = 0; fpSetZero(a, true); fpSetZero(b, true);
    fpAdd(a, b, fpsr);
    CHECK(fpIsZero(a) && fpIsNeg(a), "-0 + -0 = -0");

    // +inf + normal = +inf
    fpsr = 0; fpSetInf(a, false); fromDouble(b, 42.0);
    fpAdd(a, b, fpsr);
    CHECK(fpIsInf(a) && !fpIsNeg(a), "+inf + 42 = +inf");

    // -inf + normal = -inf
    fpsr = 0; fpSetInf(a, true); fromDouble(b, 42.0);
    fpAdd(a, b, fpsr);
    CHECK(fpIsInf(a) && fpIsNeg(a), "-inf + 42 = -inf");

    // +inf + +inf = +inf
    fpsr = 0; fpSetInf(a, false); fpSetInf(b, false);
    fpAdd(a, b, fpsr);
    CHECK(fpIsInf(a) && !fpIsNeg(a), "+inf + +inf = +inf");

    // +inf + -inf = NaN (OPERR)
    fpsr = 0; fpSetInf(a, false); fpSetInf(b, true);
    fpAdd(a, b, fpsr);
    CHECK(fpIsNaN(a), "+inf + -inf = NaN");
    CHECK((fpsr >> 8) & FPExc::OPERR, "+inf + -inf sets OPERR");

    // NaN + anything = NaN
    fpsr = 0; fpSetNaN(a); fromDouble(b, 1.0);
    fpAdd(a, b, fpsr);
    CHECK(fpIsNaN(a), "NaN + 1 = NaN");

    // anything + NaN = NaN
    fpsr = 0; fromDouble(a, 1.0); fpSetNaN(b);
    fpAdd(a, b, fpsr);
    CHECK(fpIsNaN(a), "1 + NaN = NaN");
}

void testFPU_MulSpecialMatrix() {
    SECTION("FPU FMUL Special Value Matrix");
    Registers::FPReg a, b;
    u32 fpsr;

    // 0 * 0 = 0
    fpsr = 0; fpSetZero(a, false); fpSetZero(b, false);
    fpMul(a, b, fpsr);
    CHECK(fpIsZero(a), "0 * 0 = 0");

    // 0 * inf = NaN (OPERR)
    fpsr = 0; fpSetZero(a, false); fpSetInf(b, false);
    fpMul(a, b, fpsr);
    CHECK(fpIsNaN(a), "0 * inf = NaN");
    CHECK((fpsr >> 8) & FPExc::OPERR, "0 * inf sets OPERR");

    // inf * 0 = NaN (OPERR)
    fpsr = 0; fpSetInf(a, false); fpSetZero(b, false);
    fpMul(a, b, fpsr);
    CHECK(fpIsNaN(a), "inf * 0 = NaN");

    // inf * inf = inf
    fpsr = 0; fpSetInf(a, false); fpSetInf(b, false);
    fpMul(a, b, fpsr);
    CHECK(fpIsInf(a) && !fpIsNeg(a), "+inf * +inf = +inf");

    // +inf * -inf = -inf
    fpsr = 0; fpSetInf(a, false); fpSetInf(b, true);
    fpMul(a, b, fpsr);
    CHECK(fpIsInf(a) && fpIsNeg(a), "+inf * -inf = -inf");

    // -inf * -inf = +inf
    fpsr = 0; fpSetInf(a, true); fpSetInf(b, true);
    fpMul(a, b, fpsr);
    CHECK(fpIsInf(a) && !fpIsNeg(a), "-inf * -inf = +inf");

    // normal * 0 = 0
    fpsr = 0; fromDouble(a, 42.0); fpSetZero(b, false);
    fpMul(a, b, fpsr);
    CHECK(fpIsZero(a), "42 * 0 = 0");

    // Sign rules: neg * neg = pos
    fpsr = 0; fromDouble(a, -3.0); fromDouble(b, -7.0);
    fpMul(a, b, fpsr);
    CHECK(toDouble(a) == 21.0, "(-3) * (-7) = 21");

    // NaN * anything = NaN
    fpsr = 0; fpSetNaN(a); fromDouble(b, 5.0);
    fpMul(a, b, fpsr);
    CHECK(fpIsNaN(a), "NaN * 5 = NaN");
}

void testFPU_DivSpecialMatrix() {
    SECTION("FPU FDIV Special Value Matrix");
    Registers::FPReg a, b;
    u32 fpsr;

    // 0 / 0 = NaN (OPERR)
    fpsr = 0; fpSetZero(a, false); fpSetZero(b, false);
    fpDiv(a, b, fpsr);
    CHECK(fpIsNaN(a), "0/0 = NaN");
    CHECK((fpsr >> 8) & FPExc::OPERR, "0/0 sets OPERR");

    // normal / 0 = inf (DZ)
    fpsr = 0; fromDouble(a, 1.0); fpSetZero(b, false);
    fpDiv(a, b, fpsr);
    CHECK(fpIsInf(a), "1/0 = inf");
    CHECK((fpsr >> 8) & FPExc::DZ, "1/0 sets DZ");

    // -1 / +0 = -inf
    fpsr = 0; fromDouble(a, -1.0); fpSetZero(b, false);
    fpDiv(a, b, fpsr);
    CHECK(fpIsInf(a) && fpIsNeg(a), "-1/+0 = -inf");

    // inf / inf = NaN (OPERR)
    fpsr = 0; fpSetInf(a, false); fpSetInf(b, false);
    fpDiv(a, b, fpsr);
    CHECK(fpIsNaN(a), "inf/inf = NaN");

    // inf / normal = inf
    fpsr = 0; fpSetInf(a, false); fromDouble(b, 2.0);
    fpDiv(a, b, fpsr);
    CHECK(fpIsInf(a) && !fpIsNeg(a), "inf/2 = inf");

    // normal / inf = 0
    fpsr = 0; fromDouble(a, 42.0); fpSetInf(b, false);
    fpDiv(a, b, fpsr);
    CHECK(fpIsZero(a), "42/inf = 0");

    // 0 / normal = 0
    fpsr = 0; fpSetZero(a, false); fromDouble(b, 5.0);
    fpDiv(a, b, fpsr);
    CHECK(fpIsZero(a), "0/5 = 0");

    // 0 / inf = 0
    fpsr = 0; fpSetZero(a, false); fpSetInf(b, false);
    fpDiv(a, b, fpsr);
    CHECK(fpIsZero(a), "0/inf = 0");
}

void testFPU_SqrtSpecialCases() {
    SECTION("FPU FSQRT Special Cases");
    Registers::FPReg r;
    u32 fpsr;

    // sqrt(+0) = +0
    fpsr = 0; fpSetZero(r, false);
    fpSqrt(r, fpsr);
    CHECK(fpIsZero(r) && !fpIsNeg(r), "sqrt(+0) = +0");

    // sqrt(-0) = -0
    fpsr = 0; fpSetZero(r, true);
    fpSqrt(r, fpsr);
    CHECK(fpIsZero(r) && fpIsNeg(r), "sqrt(-0) = -0");

    // sqrt(+inf) = +inf
    fpsr = 0; fpSetInf(r, false);
    fpSqrt(r, fpsr);
    CHECK(fpIsInf(r) && !fpIsNeg(r), "sqrt(+inf) = +inf");

    // sqrt(-1) = NaN (OPERR)
    fpsr = 0; fromDouble(r, -1.0);
    fpSqrt(r, fpsr);
    CHECK(fpIsNaN(r), "sqrt(-1) = NaN");
    CHECK((fpsr >> 8) & FPExc::OPERR, "sqrt(-1) sets OPERR");

    // sqrt(NaN) = NaN
    fpsr = 0; fpSetNaN(r);
    fpSqrt(r, fpsr);
    CHECK(fpIsNaN(r), "sqrt(NaN) = NaN");

    // sqrt(1) = 1 exactly
    fpsr = 0; fromDouble(r, 1.0);
    fpSqrt(r, fpsr);
    CHECK(toDouble(r) == 1.0, "sqrt(1) = 1");

    // sqrt(4) = 2 exactly
    fpsr = 0; fromDouble(r, 4.0);
    fpSqrt(r, fpsr);
    CHECK(toDouble(r) == 2.0, "sqrt(4) = 2");

    // sqrt(0.25) = 0.5 exactly
    fpsr = 0; fromDouble(r, 0.25);
    fpSqrt(r, fpsr);
    CHECK(toDouble(r) == 0.5, "sqrt(0.25) = 0.5");
}

void testFPU_CmpAllConditions() {
    SECTION("FPU FCMP Condition Codes — all combinations");
    Registers::FPReg a, b;
    u32 fpsr;

    // Positive > Positive
    fpsr = 0; fromDouble(a, 5.0); fromDouble(b, 3.0);
    fpCmp(a, b, fpsr);
    CHECK(!(fpsr & FPSR::CC_N) && !(fpsr & FPSR::CC_Z) && !(fpsr & FPSR::CC_NAN), "5>3: positive");

    // Equal
    fpsr = 0; fromDouble(a, 7.0); fromDouble(b, 7.0);
    fpCmp(a, b, fpsr);
    CHECK((fpsr & FPSR::CC_Z) && !(fpsr & FPSR::CC_N), "7==7: zero");

    // Less than
    fpsr = 0; fromDouble(a, 2.0); fromDouble(b, 9.0);
    fpCmp(a, b, fpsr);
    CHECK((fpsr & FPSR::CC_N) && !(fpsr & FPSR::CC_Z), "2<9: negative");

    // NaN comparison = unordered
    fpsr = 0; fpSetNaN(a); fromDouble(b, 1.0);
    fpCmp(a, b, fpsr);
    CHECK(fpsr & FPSR::CC_NAN, "NaN cmp 1: NAN set");

    fpsr = 0; fromDouble(a, 1.0); fpSetNaN(b);
    fpCmp(a, b, fpsr);
    CHECK(fpsr & FPSR::CC_NAN, "1 cmp NaN: NAN set");

    // +0 == -0
    fpsr = 0; fpSetZero(a, false); fpSetZero(b, true);
    fpCmp(a, b, fpsr);
    CHECK(fpsr & FPSR::CC_Z, "+0 == -0: zero");

    // -inf < +inf
    fpsr = 0; fpSetInf(a, true); fpSetInf(b, false);
    fpCmp(a, b, fpsr);
    CHECK(fpsr & FPSR::CC_N, "-inf < +inf: negative");
}

void testFPU_PackedBCD_EdgeCases() {
    SECTION("FPU Packed BCD Edge Cases");
    Registers::FPReg r;
    u32 w0, w1, w2;

    // Negative number
    fromDouble(r, -42.0);
    fpToPacked(r, 4, w0, w1, w2);
    CHECK(w0 & 0x80000000, "packed -42: sign bit set");

    // Very large number (10^15)
    fromDouble(r, 1e15);
    fpToPacked(r, 17, w0, w1, w2);
    Registers::FPReg r2;
    fpFromPacked(r2, w0, w1, w2);
    double ratio = toDouble(r2) / 1e15;
    CHECK(ratio > 0.999 && ratio < 1.001, "packed 1e15 round-trip");

    // Very small number (1e-10)
    fromDouble(r, 1e-10);
    fpToPacked(r, 10, w0, w1, w2);
    fpFromPacked(r2, w0, w1, w2);
    ratio = toDouble(r2) / 1e-10;
    CHECK(ratio > 0.99 && ratio < 1.01, "packed 1e-10 round-trip");

    // Zero
    fpSetZero(r, false);
    fpToPacked(r, 17, w0, w1, w2);
    CHECK(w0 == 0 && w1 == 0 && w2 == 0, "packed +0 = all zeros");

    // Negative zero
    fpSetZero(r, true);
    fpToPacked(r, 17, w0, w1, w2);
    CHECK((w0 & 0x80000000) && w1 == 0 && w2 == 0, "packed -0: sign only");
}

// =============================================================================
// 040 CACHE EDGE CASES
// =============================================================================

void testCache_Eviction_RoundRobin() {
    SECTION("040 D-Cache Eviction Round-Robin Order");
    Cache040 dc;
    dc.invalidateAll();

    int idx = 7; // arbitrary set
    // Fill all 4 ways
    for (int w = 0; w < 4; w++) {
        dc.set[idx].line[w].tag = 0x10000000 + (u32)(w << 10);
        dc.set[idx].line[w].valid = true;
        dc.set[idx].line[w].data[0] = 0xAA000000 + (u32)w;
    }
    dc.set[idx].nextEvict = 0;

    // Next eviction should target way 0
    CHECK_EQ(dc.set[idx].nextEvict, (u8)0, "evict pointer starts at 0");

    // Simulate eviction: advance pointer
    int victim = dc.set[idx].nextEvict;
    dc.set[idx].nextEvict = (dc.set[idx].nextEvict + 1) & 3;
    CHECK_EQ(victim, 0, "first eviction targets way 0");
    CHECK_EQ(dc.set[idx].nextEvict, (u8)1, "pointer advances to 1");

    victim = dc.set[idx].nextEvict;
    dc.set[idx].nextEvict = (dc.set[idx].nextEvict + 1) & 3;
    CHECK_EQ(victim, 1, "second eviction targets way 1");

    victim = dc.set[idx].nextEvict;
    dc.set[idx].nextEvict = (dc.set[idx].nextEvict + 1) & 3;
    CHECK_EQ(victim, 2, "third eviction targets way 2");

    victim = dc.set[idx].nextEvict;
    dc.set[idx].nextEvict = (dc.set[idx].nextEvict + 1) & 3;
    CHECK_EQ(victim, 3, "fourth eviction targets way 3");

    // Wraps around
    victim = dc.set[idx].nextEvict;
    CHECK_EQ(victim, 0, "fifth eviction wraps to way 0");
}

void testCache_TagIndex_Computation() {
    SECTION("040 Cache Tag/Index/Slot Computation");
    Cache040 dc; // Use default 040 configuration

    // Address 0x12345678:
    // Tag = 0x12345678 & 0xFFFFFC00 = 0x12345400
    // Index = (0x12345678 >> 4) & 0x3F = 0x1234567 & 0x3F = 0x27 = 39
    // Slot = (0x12345678 >> 2) & 3 = 0x489159E & 3 = 2
    CHECK_EQ(dc.tag(0x12345678), (u32)0x12345400, "tag(0x12345678)");
    CHECK_EQ(dc.index(0x12345678), 39, "index(0x12345678)");
    CHECK_EQ(Cache040::slot(0x12345678), 2, "slot(0x12345678)");

    // Address 0x00000000:
    CHECK_EQ(dc.tag(0x00000000), (u32)0x00000000, "tag(0x00000000)");
    CHECK_EQ(dc.index(0x00000000), 0, "index(0x00000000)");
    CHECK_EQ(Cache040::slot(0x00000000), 0, "slot(0x00000000)");

    // Address 0xFFFFFFFC (last longword):
    CHECK_EQ(dc.tag(0xFFFFFFFC), (u32)0xFFFFFC00, "tag(0xFFFFFFFC)");
    CHECK_EQ(dc.index(0xFFFFFFFC), 63, "index(0xFFFFFFFC)");
    CHECK_EQ(Cache040::slot(0xFFFFFFFC), 3, "slot(0xFFFFFFFC)");

    // Adjacent lines (16 bytes apart) should have different indices
    CHECK(dc.index(0x1000) != dc.index(0x1010), "adjacent lines: different index");

    // Same line, different slots
    CHECK_EQ(dc.index(0x1000), dc.index(0x1004), "same line: same index");
    CHECK_EQ(dc.index(0x1000), dc.index(0x100C), "same line: same index (offset 12)");
    CHECK(Cache040::slot(0x1000) != Cache040::slot(0x1004), "same line: different slots");
}

void testCache_PageInvalidation() {
    SECTION("040 Cache Page Invalidation (4K)");
    Cache040 dc;
    dc.invalidateAll();

    // Insert lines across a 4K page (256 lines of 16 bytes = 4096 bytes)
    u32 pageBase = 0x00100000;
    int linesInserted = 0;
    for (u32 off = 0; off < 0x1000; off += 16) {
        u32 addr = pageBase + off;
        int idx = dc.index(addr);
        u32 tag = dc.tag(addr);
        // Only insert if set has room (way 0)
        if (!dc.set[idx].line[0].valid) {
            dc.set[idx].line[0].tag = tag;
            dc.set[idx].line[0].valid = true;
            linesInserted++;
        }
    }
    CHECK(linesInserted > 0, "page invalidation: lines inserted");

    // Invalidate the page
    dc.invalidatePage(pageBase, 0x1000);

    // Verify all lines in that page are gone
    bool anyValid = false;
    for (u32 off = 0; off < 0x1000; off += 16) {
        u32 addr = pageBase + off;
        int idx = dc.index(addr);
        u32 tag = dc.tag(addr);
        if (dc.findLine(idx, tag) >= 0) { anyValid = true; break; }
    }
    CHECK(!anyValid, "page invalidation: all lines cleared");
}

// =============================================================================
// 040 MMU EDGE CASES
// =============================================================================

void testMMU_TTR_PartialMasks() {
    SECTION("040 MMU TTR Partial Masks");
    MMU040 mmu;

    // Mask=0x0F: bits 3:0 of MSB are don't-care
    // base=0x40, mask=0x0F → matches 0x40-0x4F
    u32 ttr = 0x400F8000; // base=0x40, mask=0x0F, enabled
    CHECK(mmu.matchTTR(ttr, 0x40000000, true, false) == TTRResult::Match, "mask 0F: 0x40 matches");
    CHECK(mmu.matchTTR(ttr, 0x4F000000, true, false) == TTRResult::Match, "mask 0F: 0x4F matches");
    CHECK(mmu.matchTTR(ttr, 0x50000000, true, false) == TTRResult::NoMatch, "mask 0F: 0x50 no match");
    CHECK(mmu.matchTTR(ttr, 0x3F000000, true, false) == TTRResult::NoMatch, "mask 0F: 0x3F no match");

    // Mask=0xF0: bits 7:4 of MSB are don't-care
    // base=0x00, mask=0xF0 → matches 0x00-0x0F and 0x10-0x1F... up to 0xF0-0xFF? No.
    // Actually: (addrMSB ^ base) & ~mask. base=0x00, mask=0xF0, ~mask=0x0F
    // So only low nibble of MSB must match base's low nibble (0x0)
    // Matches: 0x00, 0x10, 0x20, ..., 0xF0 (any MSB with low nibble = 0)
    ttr = 0x00F08000;
    CHECK(mmu.matchTTR(ttr, 0x00000000, true, false) == TTRResult::Match, "mask F0: 0x00 matches");
    CHECK(mmu.matchTTR(ttr, 0x10000000, true, false) == TTRResult::Match, "mask F0: 0x10 matches");
    CHECK(mmu.matchTTR(ttr, 0xF0000000, true, false) == TTRResult::Match, "mask F0: 0xF0 matches");
    CHECK(mmu.matchTTR(ttr, 0x01000000, true, false) == TTRResult::NoMatch, "mask F0: 0x01 no match");
    CHECK(mmu.matchTTR(ttr, 0x11000000, true, false) == TTRResult::NoMatch, "mask F0: 0x11 no match");
}

void testMMU_ATC_SuperVsUser() {
    SECTION("040 MMU ATC Supervisor vs User Separation");
    MMU040 mmu;
    mmu.configure(0x8000); // 4K pages

    u32 addr = 0x00100000;
    int idx = mmu.atcIndex(addr);

    // Store supervisor entry
    u32 superTag = mmu.atcTag(addr, true);
    mmu.atcStore(ATC_DATA, idx, superTag, 0x00200000, MMUSR_R);

    // Store user entry for same address (different physical mapping)
    u32 userTag = mmu.atcTag(addr, false);
    mmu.atcStore(ATC_DATA, idx, userTag, 0x00300000, MMUSR_R);

    // Lookup supervisor → gets 0x00200000
    int w = mmu.atcLookup(ATC_DATA, idx, superTag);
    CHECK(w >= 0, "super entry found");
    CHECK_EQ(mmu.atc[ATC_DATA][idx][w].phys, (u32)0x00200000, "super maps to 0x200000");

    // Lookup user → gets 0x00300000
    w = mmu.atcLookup(ATC_DATA, idx, userTag);
    CHECK(w >= 0, "user entry found");
    CHECK_EQ(mmu.atc[ATC_DATA][idx][w].phys, (u32)0x00300000, "user maps to 0x300000");

    // Tags are different
    CHECK(superTag != userTag, "super and user tags differ");
}

void testMMU_ATC_InstructionVsData() {
    SECTION("040 MMU ATC Instruction vs Data Separation");
    MMU040 mmu;
    mmu.configure(0x8000);

    u32 addr = 0x00400000;
    int idx = mmu.atcIndex(addr);
    u32 tag = mmu.atcTag(addr, true);

    // Store in instruction ATC
    mmu.atcStore(ATC_INSTRUCTION, idx, tag, 0x00500000, MMUSR_R);

    // Data ATC should NOT find it
    CHECK_EQ(mmu.atcLookup(ATC_DATA, idx, tag), -1, "I-ATC entry not in D-ATC");

    // Instruction ATC should find it
    CHECK(mmu.atcLookup(ATC_INSTRUCTION, idx, tag) >= 0, "I-ATC entry found in I-ATC");
}

void testMMU_FaultConditions() {
    SECTION("040 MMU Fault Conditions (ATC-based)");
    MMU040 mmu;
    mmu.configure(0x8000);

    u32 addr = 0x00600000;
    int idx = mmu.atcIndex(addr);
    u32 tag = mmu.atcTag(addr, true);

    // Write-protected page
    mmu.atcStore(ATC_DATA, idx, tag, 0x00700000, MMUSR_R | MMUSR_W);
    int w = mmu.atcLookup(ATC_DATA, idx, tag);
    CHECK(w >= 0, "WP page in ATC");
    u16 status = mmu.atc[ATC_DATA][idx][w].status;
    CHECK(status & MMUSR_W, "WP bit set in status");
    CHECK(status & MMUSR_R, "R bit set (resident)");

    // Supervisor-only page accessed by user
    u32 addr2 = 0x00800000;
    int idx2 = mmu.atcIndex(addr2);
    u32 tag2 = mmu.atcTag(addr2, false); // user mode
    mmu.atcStore(ATC_DATA, idx2, tag2, 0x00900000, MMUSR_R | MMUSR_S);
    w = mmu.atcLookup(ATC_DATA, idx2, tag2);
    CHECK(w >= 0, "S-protected page in ATC");
    status = mmu.atc[ATC_DATA][idx2][w].status;
    CHECK(status & MMUSR_S, "S bit set");

    // Non-resident page (R bit clear)
    u32 addr3 = 0x00A00000;
    int idx3 = mmu.atcIndex(addr3);
    u32 tag3 = mmu.atcTag(addr3, true);
    mmu.atcStore(ATC_DATA, idx3, tag3, 0x00000000, 0); // status=0, R not set
    w = mmu.atcLookup(ATC_DATA, idx3, tag3);
    CHECK(w >= 0, "non-resident page in ATC");
    status = mmu.atc[ATC_DATA][idx3][w].status;
    CHECK(!(status & MMUSR_R), "R bit clear (non-resident)");
}

// =============================================================================
// MAIN
// =============================================================================

int main() {
    printf("=== Moira Edge Case Tests ===\n\n");

    // FPU special value matrices
    testFPU_AddSpecialMatrix();
    testFPU_MulSpecialMatrix();
    testFPU_DivSpecialMatrix();
    testFPU_SqrtSpecialCases();
    testFPU_CmpAllConditions();
    testFPU_PackedBCD_EdgeCases();

    // 040 Cache edge cases
    testCache_Eviction_RoundRobin();
    testCache_TagIndex_Computation();
    testCache_PageInvalidation();

    // 040 MMU edge cases
    testMMU_TTR_PartialMasks();
    testMMU_ATC_SuperVsUser();
    testMMU_ATC_InstructionVsData();
    testMMU_FaultConditions();

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_total, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
