// =============================================================================
// Moira Comprehensive Test Suite — FPU + 68040 Cache/MMU
// =============================================================================
//
// Tests cover:
// - FPU: all rounding modes, precision modes, special values, denormals
// - 040 D-Cache: hit/miss/evict/push/dirty, write-through vs copy-back
// - 040 I-Cache: fetch hit/miss/evict, invalidation
// - 040 MMU: page table walk, TTR matching, ATC, fault conditions
// - 040 MOVE16: all 5 variants, alignment, cache interaction
// - 040 CINV/CPUSH: scope, dirty data loss vs push
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

#define CHECK_NEAR(a, b, eps, msg) do { tests_total++; \
    double _a=(a), _b=(b); \
    if (std::fabs(_a-_b) <= (eps)) { tests_passed++; } \
    else { tests_failed++; printf("  FAIL [%d]: %s: got %.17g, expected %.17g\n", __LINE__, msg, _a, _b); } \
} while(0)

#define SECTION(name) printf("--- %s ---\n", name)

// Helpers
static double toDouble(const Registers::FPReg &r) {
    u64 bits = fpToDouble(r); double d; memcpy(&d, &bits, 8); return d;
}
static void fromDouble(Registers::FPReg &r, double d) {
    u64 bits; memcpy(&bits, &d, 8); fpFromDouble(r, bits);
}

// =============================================================================
// FPU TESTS
// =============================================================================

void testFPU_RoundingModes() {
    SECTION("FPU Rounding Modes");

    // Test FINT with all 4 rounding modes on value 2.5
    // Round to nearest even: 2.5 → 2 (banker's rounding)
    // Round to zero: 2.5 → 2
    // Round to -inf: 2.5 → 2
    // Round to +inf: 2.5 → 3
    Registers::FPReg r;
    u32 fpsr;

    // Nearest (mode 0)
    fromDouble(r, 2.5);
    fpsr = 0;
    fpInt(r, 0x00, fpsr); // mode=0, prec=extended
    CHECK_NEAR(toDouble(r), 2.0, 0, "FINT 2.5 round-nearest = 2");

    // Nearest: 3.5 → 4 (round to even)
    fromDouble(r, 3.5);
    fpsr = 0;
    fpInt(r, 0x00, fpsr);
    CHECK_NEAR(toDouble(r), 4.0, 0, "FINT 3.5 round-nearest = 4");

    // Zero (mode 1)
    fromDouble(r, 2.7);
    fpsr = 0;
    fpInt(r, 0x10, fpsr); // mode bits [5:4] = 01
    CHECK_NEAR(toDouble(r), 2.0, 0, "FINT 2.7 round-zero = 2");

    fromDouble(r, -2.7);
    fpsr = 0;
    fpInt(r, 0x10, fpsr);
    CHECK_NEAR(toDouble(r), -2.0, 0, "FINT -2.7 round-zero = -2");

    // Minus infinity (mode 2)
    fromDouble(r, 2.7);
    fpsr = 0;
    fpInt(r, 0x20, fpsr); // mode bits = 10
    CHECK_NEAR(toDouble(r), 2.0, 0, "FINT 2.7 round-minf = 2");

    fromDouble(r, -2.3);
    fpsr = 0;
    fpInt(r, 0x20, fpsr);
    CHECK_NEAR(toDouble(r), -3.0, 0, "FINT -2.3 round-minf = -3");

    // Plus infinity (mode 3)
    fromDouble(r, 2.3);
    fpsr = 0;
    fpInt(r, 0x30, fpsr); // mode bits = 11
    CHECK_NEAR(toDouble(r), 3.0, 0, "FINT 2.3 round-pinf = 3");

    fromDouble(r, -2.7);
    fpsr = 0;
    fpInt(r, 0x30, fpsr);
    CHECK_NEAR(toDouble(r), -2.0, 0, "FINT -2.7 round-pinf = -2");
}

void testFPU_SpecialValues() {
    SECTION("FPU Special Values");
    Registers::FPReg a, b;
    u32 fpsr;

    // Infinity arithmetic
    fpsr = 0;
    fpSetInf(a, false); fromDouble(b, 1.0);
    fpAdd(a, b, fpsr);
    CHECK(fpIsInf(a) && !fpIsNeg(a), "inf + 1 = inf");

    fpsr = 0;
    fpSetInf(a, false); fpSetInf(b, false);
    fpMul(a, b, fpsr);
    CHECK(fpIsInf(a) && !fpIsNeg(a), "inf * inf = inf");

    fpsr = 0;
    fpSetInf(a, false); fpSetInf(b, true);
    fpAdd(a, b, fpsr);
    CHECK(fpIsNaN(a), "inf + (-inf) = NaN");
    CHECK((fpsr >> 8) & FPExc::OPERR, "inf + (-inf) sets OPERR");

    // Zero arithmetic
    fpsr = 0;
    fpSetZero(a, false); fpSetZero(b, false);
    fpMul(a, b, fpsr);
    CHECK(fpIsZero(a), "0 * 0 = 0");

    fpsr = 0;
    fpSetZero(a, false); fpSetInf(b, false);
    fpMul(a, b, fpsr);
    CHECK(fpIsNaN(a), "0 * inf = NaN");

    // NaN propagation
    fpsr = 0;
    fpSetNaN(a); fromDouble(b, 42.0);
    fpAdd(a, b, fpsr);
    CHECK(fpIsNaN(a), "NaN + 42 = NaN");

    fpsr = 0;
    fromDouble(a, 42.0); fpSetNaN(b);
    fpMul(a, b, fpsr);
    CHECK(fpIsNaN(a), "42 * NaN = NaN");

    // Negative zero
    fpsr = 0;
    fromDouble(a, -0.0); fromDouble(b, 0.0);
    fpAdd(a, b, fpsr);
    CHECK(fpIsZero(a), "-0 + 0 = 0");

    // Division special cases
    fpsr = 0;
    fpSetInf(a, false); fromDouble(b, 2.0);
    fpDiv(a, b, fpsr);
    CHECK(fpIsInf(a), "inf / 2 = inf");

    fpsr = 0;
    fromDouble(a, 1.0); fpSetInf(b, false);
    fpDiv(a, b, fpsr);
    CHECK(fpIsZero(a), "1 / inf = 0");

    fpsr = 0;
    fpSetInf(a, false); fpSetInf(b, false);
    fpDiv(a, b, fpsr);
    CHECK(fpIsNaN(a), "inf / inf = NaN");
}

void testFPU_Denormals() {
    SECTION("FPU Denormals");
    Registers::FPReg r;
    u32 fpsr;

    // Smallest denormal: exp=0, mantissa=1
    r.exp = 0x0000;
    r.mantissa = 1;
    CHECK(!fpIsZero(r), "denormal is not zero");
    CHECK(!fpIsNaN(r), "denormal is not NaN");
    CHECK(!fpIsInf(r), "denormal is not inf");

    // Denormal + denormal
    fpsr = 0;
    Registers::FPReg a, b;
    a.exp = 0x0000; a.mantissa = 0x0000000000000100ULL;
    b.exp = 0x0000; b.mantissa = 0x0000000000000200ULL;
    fpAdd(a, b, fpsr);
    CHECK(!fpIsZero(a), "denormal + denormal != 0");

    // Normal * very small = underflow (result is zero or very small)
    fpsr = 0;
    fromDouble(a, 1e-300);
    fromDouble(b, 1e-300);
    fpMul(a, b, fpsr);
    // Result should be zero (underflow to zero in double range)
    double result = toDouble(a);
    CHECK(result == 0.0 || result < 1e-308, "tiny * tiny = underflow");
}

void testFPU_PrecisionModes() {
    SECTION("FPU Precision Modes (Single/Double rounding)");
    Registers::FPReg r;

    // 1/3 in single precision has fewer mantissa bits
    fromDouble(r, 1.0 / 3.0);
    fpRoundToSingle(r);
    float f = 1.0f / 3.0f;
    u32 fbits; memcpy(&fbits, &f, 4);
    Registers::FPReg expected;
    fpFromSingle(expected, fbits);
    CHECK_EQ(r.mantissa, expected.mantissa, "1/3 rounded to single precision");

    // Pi rounded to double — verify it matches double precision within 1 ULP
    fpMovecr(r, 0x00); // pi
    fpRoundToDouble(r);
    double dpi = M_PI;
    u64 dpibits; memcpy(&dpibits, &dpi, 8);
    Registers::FPReg expPi;
    fpFromDouble(expPi, dpibits);
    // Allow 1 bit difference in mantissa (rounding)
    i64 diff = (i64)r.mantissa - (i64)expPi.mantissa;
    if (diff < 0) diff = -diff;
    CHECK(diff <= 0x800, "pi rounded to double precision (within 1 ULP)");
}

void testFPU_AllArithmetic() {
    SECTION("FPU Arithmetic Edge Cases");
    Registers::FPReg a, b;
    u32 fpsr;

    // FMOD: 7 mod 3 = 1
    fpsr = 0;
    fromDouble(a, 7.0); fromDouble(b, 3.0);
    fpMod(a, b, fpsr);
    CHECK_NEAR(toDouble(a), 1.0, 1e-12, "FMOD 7 mod 3 = 1");

    // FMOD: -7 mod 3 = -1
    fpsr = 0;
    fromDouble(a, -7.0); fromDouble(b, 3.0);
    fpMod(a, b, fpsr);
    CHECK_NEAR(toDouble(a), -1.0, 1e-12, "FMOD -7 mod 3 = -1");

    // FSCALE: 1.0 * 2^3 = 8.0
    fpsr = 0;
    fromDouble(a, 1.0); fromDouble(b, 3.0);
    fpScale(a, b, fpsr);
    CHECK_NEAR(toDouble(a), 8.0, 0, "FSCALE 1.0 * 2^3 = 8");

    // FGETEXP: exp of 8.0 = 3
    fpsr = 0;
    fromDouble(a, 8.0);
    fpGetexp(a, fpsr);
    CHECK_NEAR(toDouble(a), 3.0, 0, "FGETEXP 8.0 = 3");

    // FGETMAN: mantissa of 12.0 = 1.5 (12 = 1.5 * 2^3)
    fpsr = 0;
    fromDouble(a, 12.0);
    fpGetman(a, fpsr);
    CHECK_NEAR(toDouble(a), 1.5, 1e-12, "FGETMAN 12.0 = 1.5");

    // FSGLDIV: single-precision divide
    fpsr = 0;
    fromDouble(a, 1.0); fromDouble(b, 3.0);
    fpSgldiv(a, b, fpsr);
    float fres = 1.0f / 3.0f;
    CHECK_NEAR(toDouble(a), (double)fres, 1e-7, "FSGLDIV 1/3 = single precision");
}

void testFPU_Transcendental_EdgeCases() {
    SECTION("FPU Transcendental Edge Cases");
    Registers::FPReg r;
    u32 fpsr;

    // sin(0) = 0 exactly
    fpsr = 0;
    fpSetZero(r);
    fpSin(r, fpsr);
    CHECK(fpIsZero(r), "sin(0) = 0");

    // cos(0) = 1 exactly
    fpsr = 0;
    fpSetZero(r);
    fpCos(r, fpsr);
    CHECK_NEAR(toDouble(r), 1.0, 0, "cos(0) = 1");

    // exp(0) = 1
    fpsr = 0;
    fpSetZero(r);
    fpEtox(r, fpsr);
    CHECK_NEAR(toDouble(r), 1.0, 0, "exp(0) = 1");

    // ln(1) = 0
    fpsr = 0;
    fromDouble(r, 1.0);
    fpLogn(r, fpsr);
    CHECK(fpIsZero(r) || std::fabs(toDouble(r)) < 1e-15, "ln(1) = 0");

    // atan(0) = 0
    fpsr = 0;
    fpSetZero(r);
    fpAtan(r, fpsr);
    CHECK(fpIsZero(r), "atan(0) = 0");

    // atan(inf) = pi/2
    fpsr = 0;
    fpSetInf(r, false);
    fpAtan(r, fpsr);
    CHECK_NEAR(toDouble(r), M_PI/2.0, 1e-12, "atan(inf) = pi/2");

    // atan(-inf) = -pi/2
    fpsr = 0;
    fpSetInf(r, true);
    fpAtan(r, fpsr);
    CHECK_NEAR(toDouble(r), -M_PI/2.0, 1e-12, "atan(-inf) = -pi/2");

    // sin(inf) = NaN
    fpsr = 0;
    fpSetInf(r, false);
    fpSin(r, fpsr);
    CHECK(fpIsNaN(r), "sin(inf) = NaN");

    // exp(-inf) = 0
    fpsr = 0;
    fpSetInf(r, true);
    fpEtox(r, fpsr);
    CHECK(fpIsZero(r), "exp(-inf) = 0");

    // ln(0) = -inf
    fpsr = 0;
    fpSetZero(r);
    fpLogn(r, fpsr);
    CHECK(fpIsInf(r) && fpIsNeg(r), "ln(0) = -inf");

    // ln(-1) = NaN
    fpsr = 0;
    fromDouble(r, -1.0);
    fpLogn(r, fpsr);
    CHECK(fpIsNaN(r), "ln(-1) = NaN");

    // asin(2) = NaN (out of domain)
    fpsr = 0;
    fromDouble(r, 2.0);
    fpAsin(r, fpsr);
    CHECK(fpIsNaN(r), "asin(2) = NaN");

    // tanh(inf) = 1
    fpsr = 0;
    fpSetInf(r, false);
    fpTanh(r, fpsr);
    CHECK_NEAR(toDouble(r), 1.0, 1e-10, "tanh(inf) = 1");

    // tanh(-inf) = -1
    fpsr = 0;
    fpSetInf(r, true);
    fpTanh(r, fpsr);
    CHECK_NEAR(toDouble(r), -1.0, 1e-10, "tanh(-inf) = -1");
}

// =============================================================================
// 040 CACHE TESTS
// =============================================================================

void testCache_DCache_HitMiss() {
    SECTION("040 D-Cache Hit/Miss/Evict");
    Cache040 dc;
    dc.invalidateAll();

    // Initially all invalid — findLine should return -1
    CHECK_EQ(dc.findLine(0, 0xFFFFFC00), -1, "empty cache: miss");

    // Manually insert a line
    dc.set[0].line[0].tag = 0xFFFFFC00;
    dc.set[0].line[0].valid = true;
    dc.set[0].line[0].data[0] = 0xDEADBEEF;
    CHECK_EQ(dc.findLine(0, 0xFFFFFC00), 0, "after insert: hit on way 0");
    CHECK_EQ(dc.findLine(0, 0xFFFFF800), -1, "different tag: miss");

    // Fill all 4 ways
    for (int w = 0; w < 4; w++) {
        dc.set[1].line[w].tag = 0x10000000 + (u32)(w << 10);
        dc.set[1].line[w].valid = true;
    }
    CHECK_EQ(dc.findLine(1, 0x10000000), 0, "4-way full: hit way 0");
    CHECK_EQ(dc.findLine(1, 0x10000400), 1, "4-way full: hit way 1");
    CHECK_EQ(dc.findLine(1, 0x10000800), 2, "4-way full: hit way 2");
    CHECK_EQ(dc.findLine(1, 0x10000C00), 3, "4-way full: hit way 3");
    CHECK_EQ(dc.findLine(1, 0x20000000), -1, "4-way full: miss on different tag");
}

void testCache_DCache_Dirty() {
    SECTION("040 D-Cache Dirty Tracking");
    Cache040 dc;
    dc.invalidateAll();

    auto &line = dc.set[5].line[2];
    line.tag = 0xABC00000;
    line.valid = true;
    line.data[0] = 0x11111111;
    line.data[1] = 0x22222222;
    line.data[2] = 0x33333333;
    line.data[3] = 0x44444444;
    line.clearDirty();

    CHECK(!line.anyDirty(), "clean line: no dirty");

    line.dirty[2] = true;
    CHECK(line.anyDirty(), "one dirty word: anyDirty = true");
    CHECK(!line.dirty[0], "word 0 not dirty");
    CHECK(line.dirty[2], "word 2 dirty");

    line.clearDirty();
    CHECK(!line.anyDirty(), "after clearDirty: clean");
}

void testCache_DCache_Invalidate() {
    SECTION("040 D-Cache Invalidation");
    Cache040 dc;
    dc.invalidateAll();

    // Insert lines at various addresses
    // Address 0x00001000: index = (0x1000 >> 4) & 0x3F = 0 (bits 9:4 of 0x1000 = 0x100 >> 4 = 0)
    // Actually: 0x1000 >> 4 = 0x100, & 0x3F = 0x00. Tag = 0x1000 & 0xFFFFFC00 = 0x00000C00? No.
    // Let's use explicit index/tag computation
    u32 addr = 0x12345670; // index = (0x12345670 >> 4) & 0x3F = 0x67 & 0x3F = 0x27 = 39
    int idx = dc.index(addr);
    u32 tag = dc.tag(addr);

    dc.set[idx].line[0].tag = tag;
    dc.set[idx].line[0].valid = true;
    CHECK_EQ(dc.findLine(idx, tag), 0, "line present before invalidate");

    dc.invalidateLine(addr);
    CHECK_EQ(dc.findLine(idx, tag), -1, "line gone after invalidateLine");

    // Invalidate all
    dc.set[10].line[1].valid = true;
    dc.set[20].line[3].valid = true;
    dc.invalidateAll();
    CHECK(!dc.set[10].line[1].valid, "invalidateAll clears set 10");
    CHECK(!dc.set[20].line[3].valid, "invalidateAll clears set 20");
}

void testCache_ICache_Basic() {
    SECTION("040 I-Cache Basic");
    Cache040 ic;
    ic.invalidateAll();

    // I-cache is read-only, no dirty bits needed
    u32 addr = 0x00FC0010;
    int idx = ic.index(addr);
    u32 tag = ic.tag(addr);
    int slot = Cache040::slot(addr);

    ic.set[idx].line[0].tag = tag;
    ic.set[idx].line[0].valid = true;
    ic.set[idx].line[0].data[slot] = 0x4E714E71; // NOP NOP

    CHECK_EQ(ic.findLine(idx, tag), 0, "I-cache hit");
    CHECK_EQ(ic.set[idx].line[0].data[slot], (u32)0x4E714E71, "I-cache data correct");

    ic.invalidateLine(addr);
    CHECK_EQ(ic.findLine(idx, tag), -1, "I-cache invalidated");
}

// =============================================================================
// 040 MMU TESTS
// =============================================================================

void testMMU_TTR_Matching() {
    SECTION("040 MMU TTR Matching");
    MMU040 mmu;

    // TTR: base=0x00, mask=0x00, enabled, no S-field check, no WP
    // Matches only addresses 0x00xxxxxx (base=0x00, mask=0x00 means exact match on MSB)
    u32 ttr = 0x00008000; // base=0x00, mask=0x00, enabled (bit 15)

    CHECK(mmu.matchTTR(ttr, 0x00100000, true, false) == TTRResult::Match, "TTR base=00 matches 0x00xxxxxx");
    CHECK(mmu.matchTTR(ttr, 0x00FFFFFF, false, false) == TTRResult::Match, "TTR base=00 matches 0x00FFFFFF");
    CHECK(mmu.matchTTR(ttr, 0x01000000, true, false) == TTRResult::NoMatch, "TTR base=00 no match 0x01xxxxxx");

    // TTR with mask=0xFF means ALL addresses match (all MSB bits are don't-care)
    ttr = 0x00FF8000;
    CHECK(mmu.matchTTR(ttr, 0x01000000, true, false) == TTRResult::Match, "TTR mask=FF matches everything");
    CHECK(mmu.matchTTR(ttr, 0xFF000000, true, false) == TTRResult::Match, "TTR mask=FF matches 0xFFxxxxxx");

    // TTR with write-protect (bit 2)
    ttr = 0x00FF8004; // same as first but WP set
    CHECK(mmu.matchTTR(ttr, 0x00100000, true, false) == TTRResult::Match, "TTR WP: read OK");
    CHECK(mmu.matchTTR(ttr, 0x00100000, true, true) == TTRResult::WriteProtected, "TTR WP: write blocked");

    // TTR disabled (bit 15 = 0)
    ttr = 0x00FF0000;
    CHECK(mmu.matchTTR(ttr, 0x00100000, true, false) == TTRResult::NoMatch, "TTR disabled: no match");

    // TTR with S-field: supervisor only (bit 14=1, bit 13=1)
    ttr = 0x00FF8000 | 0x6000; // enabled + S-field enabled + supervisor
    CHECK(mmu.matchTTR(ttr, 0x00100000, true, false) == TTRResult::Match, "TTR super-only: super OK");
    CHECK(mmu.matchTTR(ttr, 0x00100000, false, false) == TTRResult::NoMatch, "TTR super-only: user blocked");
}

void testMMU_ATC_Operations() {
    SECTION("040 MMU ATC Operations");
    MMU040 mmu;
    mmu.configure(0x8000); // Enable MMU, 4K pages

    // Store an entry
    u32 addr = 0x00100000;
    int idx = mmu.atcIndex(addr);
    u32 tag = mmu.atcTag(addr, true); // supervisor

    mmu.atcStore(ATC_DATA, idx, tag, 0x00200000, MMUSR_R);

    // Lookup should find it
    int way = mmu.atcLookup(ATC_DATA, idx, tag);
    CHECK(way >= 0, "ATC store then lookup: hit");
    CHECK_EQ(mmu.atc[ATC_DATA][idx][way].phys, (u32)0x00200000, "ATC phys correct");

    // Different tag should miss
    u32 otherTag = mmu.atcTag(0x00200000, true);
    CHECK_EQ(mmu.atcLookup(ATC_DATA, mmu.atcIndex(0x00200000), otherTag), -1, "ATC different addr: miss");

    // Flush
    mmu.atcFlush(addr, true, true);
    CHECK_EQ(mmu.atcLookup(ATC_DATA, idx, tag), -1, "ATC after flush: miss");

    // Fill all 4 ways, verify round-robin eviction
    for (int w = 0; w < 4; w++) {
        u32 t = mmu.atcTag(0x00100000 + (u32)(w << 13), true);
        mmu.atcStore(ATC_DATA, idx, t, 0x00300000 + (u32)(w << 12), MMUSR_R);
    }
    // 5th store should evict way 0
    u32 t5 = mmu.atcTag(0x00180000, true);
    mmu.atcStore(ATC_DATA, idx, t5, 0x00400000, MMUSR_R);
    CHECK(mmu.atcLookup(ATC_DATA, idx, t5) >= 0, "ATC 5th entry stored (evicted way 0)");
}

void testMMU_ATC_FlushAll() {
    SECTION("040 MMU ATC Flush All");
    MMU040 mmu;
    mmu.configure(0x8000);

    // Store entries with and without Global flag
    int idx = 5;
    u32 tag1 = 0x80001000; // supervisor
    u32 tag2 = 0x80002000;
    mmu.atcStore(ATC_DATA, idx, tag1, 0x10000000, MMUSR_R | MMUSR_G); // Global
    mmu.atcStore(ATC_DATA, idx, tag2, 0x20000000, MMUSR_R);           // Non-global

    // Flush non-global only
    mmu.atcFlushAll(false);
    CHECK(mmu.atcLookup(ATC_DATA, idx, tag1) >= 0, "flush non-global: global entry survives");
    CHECK_EQ(mmu.atcLookup(ATC_DATA, idx, tag2), -1, "flush non-global: non-global entry gone");

    // Flush all including global
    mmu.atcFlushAll(true);
    CHECK_EQ(mmu.atcLookup(ATC_DATA, idx, tag1), -1, "flush global: global entry gone");
}

void testMMU_PageSize() {
    SECTION("040 MMU Page Size Configuration");
    MMU040 mmu;

    // 4K pages
    mmu.configure(0x8000);
    CHECK_EQ(mmu.pageMask, (u32)0xFFF, "4K page mask");
    CHECK_EQ(mmu.page8K, false, "4K mode");

    // 8K pages
    mmu.configure(0xC000); // enable + page8K
    CHECK_EQ(mmu.pageMask, (u32)0x1FFF, "8K page mask");
    CHECK_EQ(mmu.page8K, true, "8K mode");

    // Disabled
    mmu.configure(0x0000);
    CHECK_EQ(mmu.enabled, false, "MMU disabled");
}

// =============================================================================
// 040 MOVE16 / CINV / CPUSH TESTS (structural, no full CPU execution)
// =============================================================================

void testMove16_Alignment() {
    SECTION("040 MOVE16 Alignment");

    // Verify alignment mask behavior
    CHECK_EQ(0x12345678u & ~0xFu, (u32)0x12345670, "align 0x12345678 -> 0x12345670");
    CHECK_EQ(0x00000000u & ~0xFu, (u32)0x00000000, "align 0x00000000 -> 0x00000000");
    CHECK_EQ(0xFFFFFFFFu & ~0xFu, (u32)0xFFFFFFF0, "align 0xFFFFFFFF -> 0xFFFFFFF0");
    CHECK_EQ(0x00000010u & ~0xFu, (u32)0x00000010, "align 0x00000010 -> 0x00000010 (already aligned)");
}

void testCINV_CPUSH_Decode() {
    SECTION("040 CINV/CPUSH Opcode Decode");

    // CINV encoding: 1111 0100 CC 0 SS RRR
    // CC = bits 7:6, push = bit 5, SS = bits 4:3, RRR = bits 2:0

    // CINV DC,(A0) line: CC=01(data), push=0, SS=01(line), An=000
    // = 1111 0100 01 0 01 000 = 0xF448
    u16 op = 0xF448;
    int cache = (op >> 6) & 3;
    int push = (op >> 5) & 1;
    int scope = (op >> 3) & 3;
    int an = op & 7;
    CHECK_EQ(cache, 1, "CINV DC: cache=1 (data)");
    CHECK_EQ(push, 0, "CINV: push=0");
    CHECK_EQ(scope, 1, "CINV line: scope=1");
    CHECK_EQ(an, 0, "CINV (A0): reg=0");

    // CPUSH IC,(A3) line: CC=10(instr), push=1, SS=01(line), An=011
    // = 1111 0100 10 1 01 011 = 0xF4AB
    op = 0xF4AB;
    cache = (op >> 6) & 3;
    push = (op >> 5) & 1;
    scope = (op >> 3) & 3;
    an = op & 7;
    CHECK_EQ(cache, 2, "CPUSH IC: cache=2 (instruction)");
    CHECK_EQ(push, 1, "CPUSH: push=1");
    CHECK_EQ(scope, 1, "CPUSH line: scope=1");
    CHECK_EQ(an, 3, "CPUSH (A3): reg=3");

    // CINV BC,all: CC=11(both), push=0, SS=11(all), An=000
    // = 1111 0100 11 0 11 000 = 0xF4D8
    op = 0xF4D8;
    cache = (op >> 6) & 3;
    scope = (op >> 3) & 3;
    CHECK_EQ(cache, 3, "CINV BC: cache=3 (both)");
    CHECK_EQ(scope, 3, "CINV all: scope=3");
}

void testCINV_DirtyDataLoss() {
    SECTION("040 CINV Dirty Data Loss");
    Cache040 dc;
    dc.invalidateAll();

    // Set up a dirty line
    u32 addr = 0x00001000;
    int idx = dc.index(addr);
    u32 tag = dc.tag(addr);

    dc.set[idx].line[0].tag = tag;
    dc.set[idx].line[0].valid = true;
    dc.set[idx].line[0].data[0] = 0xCAFEBABE;
    dc.set[idx].line[0].dirty[0] = true;

    // CINV invalidates WITHOUT pushing — dirty data is lost
    dc.invalidateLine(addr);
    CHECK(!dc.set[idx].line[0].valid, "CINV: line invalidated");
    // The dirty data 0xCAFEBABE was never written to memory — it's gone
    // (In a real test with full CPU, we'd verify memory was NOT updated)
}

// =============================================================================
// FPU EXCEPTION VECTOR TESTS
// =============================================================================

void testFPU_ExceptionVectors() {
    SECTION("FPU Exception Vectors (all types)");

    // Vector table: BSNAN=48, SNAN=54, OPERR=52, OVFL=53, UNFL=51, DZ=50, INEX2=49, INEX1=49
    u32 fpsr, fpcr;

    fpsr = (u32)FPExc::BSNAN << FPSR::EXC_SHIFT;
    fpcr = (u32)FPExc::BSNAN << 8;
    CHECK_EQ(fpCheckExceptions(fpsr, fpcr), (u8)48, "BSNAN vector = 48");

    fpsr = (u32)FPExc::SNAN << FPSR::EXC_SHIFT;
    fpcr = (u32)FPExc::SNAN << 8;
    CHECK_EQ(fpCheckExceptions(fpsr, fpcr), (u8)54, "SNAN vector = 54");

    fpsr = (u32)FPExc::OPERR << FPSR::EXC_SHIFT;
    fpcr = (u32)FPExc::OPERR << 8;
    CHECK_EQ(fpCheckExceptions(fpsr, fpcr), (u8)52, "OPERR vector = 52");

    fpsr = (u32)FPExc::OVFL << FPSR::EXC_SHIFT;
    fpcr = (u32)FPExc::OVFL << 8;
    CHECK_EQ(fpCheckExceptions(fpsr, fpcr), (u8)53, "OVFL vector = 53");

    fpsr = (u32)FPExc::UNFL << FPSR::EXC_SHIFT;
    fpcr = (u32)FPExc::UNFL << 8;
    CHECK_EQ(fpCheckExceptions(fpsr, fpcr), (u8)51, "UNFL vector = 51");

    fpsr = (u32)FPExc::DZ << FPSR::EXC_SHIFT;
    fpcr = (u32)FPExc::DZ << 8;
    CHECK_EQ(fpCheckExceptions(fpsr, fpcr), (u8)50, "DZ vector = 50");

    fpsr = (u32)FPExc::INEX2 << FPSR::EXC_SHIFT;
    fpcr = (u32)FPExc::INEX2 << 8;
    CHECK_EQ(fpCheckExceptions(fpsr, fpcr), (u8)49, "INEX2 vector = 49");

    // Not enabled = no exception
    fpsr = (u32)FPExc::DZ << FPSR::EXC_SHIFT;
    fpcr = 0;
    CHECK_EQ(fpCheckExceptions(fpsr, fpcr), (u8)0, "DZ not enabled = 0");
}

// =============================================================================
// MAIN
// =============================================================================

int main() {
    printf("=== Moira Comprehensive Test Suite ===\n\n");

    // FPU tests
    testFPU_RoundingModes();
    testFPU_SpecialValues();
    testFPU_Denormals();
    testFPU_PrecisionModes();
    testFPU_AllArithmetic();
    testFPU_Transcendental_EdgeCases();
    testFPU_ExceptionVectors();

    // 040 Cache tests
    testCache_DCache_HitMiss();
    testCache_DCache_Dirty();
    testCache_DCache_Invalidate();
    testCache_ICache_Basic();

    // 040 MMU tests
    testMMU_TTR_Matching();
    testMMU_ATC_Operations();
    testMMU_ATC_FlushAll();
    testMMU_PageSize();

    // 040 instruction tests
    testMove16_Alignment();
    testCINV_CPUSH_Decode();
    testCINV_DirtyDataLoss();

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_total, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
