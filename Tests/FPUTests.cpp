// FPU Golden Vector Tests
// Test vectors derived from IEEE 754 mathematical identities and known results.
// WinUAE's SoftFloat implementation produces identical results for these cases.

#include "MoiraFPU.h"
#include <cstdio>
#include <cmath>
#include <cstring>

using namespace moira;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { tests_passed++; } \
    else { tests_failed++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

#define CHECK_FP(r, expExp, expMant, msg) do { \
    if ((r).exp == (u16)(expExp) && (r).mantissa == (u64)(expMant)) { tests_passed++; } \
    else { tests_failed++; printf("FAIL: %s: got {0x%04X, 0x%016llX}, expected {0x%04X, 0x%016llX} (line %d)\n", \
        msg, (r).exp, (unsigned long long)(r).mantissa, (u16)(expExp), (unsigned long long)(expMant), __LINE__); } \
} while(0)

#define CHECK_NEAR(a, b, eps, msg) do { \
    double _a = (a), _b = (b), _e = (eps); \
    if (std::fabs(_a - _b) <= _e) { tests_passed++; } \
    else { tests_failed++; printf("FAIL: %s: got %.17g, expected %.17g (line %d)\n", msg, _a, _b, __LINE__); } \
} while(0)

static double toDouble(const Registers::FPReg &r) {
    u64 bits = fpToDouble(r);
    double d; memcpy(&d, &bits, 8);
    return d;
}

static void fromDouble(Registers::FPReg &r, double d) {
    u64 bits; memcpy(&bits, &d, 8);
    fpFromDouble(r, bits);
}

// ---- Format Conversion Golden Vectors ----

void testConversions() {
    Registers::FPReg r;

    // Integer round-trips
    fpFromLong(r, 42);
    CHECK(fpToLong(r, 0) == 42, "long round-trip 42");
    fpFromLong(r, -1000);
    CHECK(fpToLong(r, 0) == -1000, "long round-trip -1000");
    fpFromLong(r, 0);
    CHECK(fpIsZero(r), "zero from long");
    fpFromWord(r, 32767);
    CHECK(fpToWord(r, 0) == 32767, "word max");
    fpFromByte(r, -128);
    CHECK(fpToByte(r, 0) == -128, "byte min");

    // Single precision: exact representation of 1.0
    u32 one_f32 = 0x3F800000; // IEEE 754 single 1.0
    fpFromSingle(r, one_f32);
    CHECK_FP(r, 0x3FFF, 0x8000000000000000ULL, "single 1.0 -> extended");
    CHECK(fpToSingle(r) == one_f32, "extended 1.0 -> single");

    // Double precision: exact representation of -2.0
    u64 neg2_f64 = 0xC000000000000000ULL;
    fpFromDouble(r, neg2_f64);
    CHECK_FP(r, 0xC000, 0x8000000000000000ULL, "double -2.0 -> extended");
    CHECK(fpToDouble(r) == neg2_f64, "extended -2.0 -> double");

    // Special values
    fpSetZero(r, false);
    CHECK(fpIsZero(r) && !fpIsNeg(r), "positive zero");
    fpSetZero(r, true);
    CHECK(fpIsZero(r) && fpIsNeg(r), "negative zero");
    fpSetInf(r, false);
    CHECK(fpIsInf(r) && !fpIsNeg(r), "positive infinity");
    fpSetInf(r, true);
    CHECK(fpIsInf(r) && fpIsNeg(r), "negative infinity");
    fpSetNaN(r);
    CHECK(fpIsNaN(r), "NaN");
}

// ---- FMOVECR Constant ROM Golden Vectors ----

void testConstants() {
    Registers::FPReg r;

    // Exact bit patterns from Motorola MC68881 ROM
    fpMovecr(r, 0x00);
    CHECK_FP(r, 0x4000, 0xC90FDAA22168C235ULL, "FMOVECR pi bits");
    CHECK_NEAR(toDouble(r), M_PI, 1e-15, "FMOVECR pi value");

    fpMovecr(r, 0x0C);
    CHECK_FP(r, 0x4000, 0xADF85458A2BB4A9AULL, "FMOVECR e bits");
    CHECK_NEAR(toDouble(r), M_E, 1e-15, "FMOVECR e value");

    fpMovecr(r, 0x30);
    CHECK_FP(r, 0x3FFE, 0xB17217F7D1CF79ACULL, "FMOVECR ln2 bits");
    fpMovecr(r, 0x31);
    CHECK_FP(r, 0x4000, 0x935D8DDDAAA8AC17ULL, "FMOVECR ln10 bits");
    fpMovecr(r, 0x0D);
    CHECK_FP(r, 0x3FFF, 0xB8AA3B295C17F0BCULL, "FMOVECR log2e bits");
    fpMovecr(r, 0x0F);
    CHECK(fpIsZero(r), "FMOVECR zero");
    fpMovecr(r, 0x32);
    CHECK_FP(r, 0x3FFF, 0x8000000000000000ULL, "FMOVECR 1.0 bits");
    fpMovecr(r, 0x33);
    CHECK_NEAR(toDouble(r), 10.0, 0, "FMOVECR 10.0");
    fpMovecr(r, 0x34);
    CHECK_NEAR(toDouble(r), 100.0, 0, "FMOVECR 100.0");
}

// ---- Arithmetic Golden Vectors ----

void testArithmetic() {
    Registers::FPReg a, b;
    u32 fpsr = 0;

    // ADD: 1.5 + 2.5 = 4.0 (exact)
    fromDouble(a, 1.5); fromDouble(b, 2.5);
    fpAdd(a, b, fpsr);
    CHECK_NEAR(toDouble(a), 4.0, 0, "FADD 1.5+2.5");

    // SUB: 10.0 - 3.0 = 7.0 (exact)
    fromDouble(a, 10.0); fromDouble(b, 3.0);
    fpSub(a, b, fpsr);
    CHECK_NEAR(toDouble(a), 7.0, 0, "FSUB 10-3");

    // MUL: 6.0 * 7.0 = 42.0 (exact)
    fromDouble(a, 6.0); fromDouble(b, 7.0);
    fpMul(a, b, fpsr);
    CHECK_NEAR(toDouble(a), 42.0, 0, "FMUL 6*7");

    // DIV: 1.0 / 3.0 (inexact, check precision)
    fromDouble(a, 1.0); fromDouble(b, 3.0);
    fpDiv(a, b, fpsr);
    CHECK_NEAR(toDouble(a), 1.0/3.0, 1e-15, "FDIV 1/3");

    // DIV by zero -> infinity + DZ exception
    fpsr = 0;
    fromDouble(a, 1.0); fpSetZero(b);
    fpDiv(a, b, fpsr);
    CHECK(fpIsInf(a) && !fpIsNeg(a), "FDIV 1/0 = +inf");
    CHECK((fpsr >> 8) & FPExc::DZ, "FDIV 1/0 sets DZ");

    // -1/0 -> -infinity
    fpsr = 0;
    fromDouble(a, -1.0); fpSetZero(b);
    fpDiv(a, b, fpsr);
    CHECK(fpIsInf(a) && fpIsNeg(a), "FDIV -1/0 = -inf");

    // 0/0 -> NaN + OPERR
    fpsr = 0;
    fpSetZero(a); fpSetZero(b);
    fpDiv(a, b, fpsr);
    CHECK(fpIsNaN(a), "FDIV 0/0 = NaN");
    CHECK((fpsr >> 8) & FPExc::OPERR, "FDIV 0/0 sets OPERR");

    // SQRT(4.0) = 2.0 (exact)
    fromDouble(a, 4.0);
    fpSqrt(a, fpsr);
    CHECK_NEAR(toDouble(a), 2.0, 0, "FSQRT 4");

    // SQRT(2.0) (inexact, verify precision)
    fromDouble(a, 2.0);
    fpSqrt(a, fpsr);
    CHECK_NEAR(toDouble(a), std::sqrt(2.0), 1e-15, "FSQRT 2");

    // SQRT(-1) -> NaN + OPERR
    fpsr = 0;
    fromDouble(a, -1.0);
    fpSqrt(a, fpsr);
    CHECK(fpIsNaN(a), "FSQRT -1 = NaN");
    CHECK((fpsr >> 8) & FPExc::OPERR, "FSQRT -1 sets OPERR");

    // ABS/NEG
    fromDouble(a, -42.0);
    fpAbs(a, fpsr);
    CHECK_NEAR(toDouble(a), 42.0, 0, "FABS -42");
    fromDouble(a, 42.0);
    fpNeg(a, fpsr);
    CHECK_NEAR(toDouble(a), -42.0, 0, "FNEG 42");

    // CMP
    fpsr = 0;
    fromDouble(a, 5.0); fromDouble(b, 3.0);
    fpCmp(a, b, fpsr);
    CHECK(!(fpsr & FPSR::CC_N) && !(fpsr & FPSR::CC_Z), "FCMP 5>3");

    fpsr = 0;
    fromDouble(a, 3.0); fromDouble(b, 3.0);
    fpCmp(a, b, fpsr);
    CHECK(fpsr & FPSR::CC_Z, "FCMP 3==3");

    fpsr = 0;
    fromDouble(a, 1.0); fromDouble(b, 5.0);
    fpCmp(a, b, fpsr);
    CHECK(fpsr & FPSR::CC_N, "FCMP 1<5");

    // CMP with NaN -> unordered
    fpsr = 0;
    fromDouble(a, 1.0); fpSetNaN(b);
    fpCmp(a, b, fpsr);
    CHECK(fpsr & FPSR::CC_NAN, "FCMP NaN unordered");
}

// ---- Transcendental Golden Vectors ----

void testTranscendental() {
    Registers::FPReg r;
    u32 fpsr = 0;

    fromDouble(r, 0.0); fpSin(r, fpsr);
    CHECK_NEAR(toDouble(r), 0.0, 1e-15, "FSIN 0");
    fromDouble(r, M_PI / 2.0); fpSin(r, fpsr);
    CHECK_NEAR(toDouble(r), 1.0, 1e-12, "FSIN pi/2");
    fromDouble(r, 0.0); fpCos(r, fpsr);
    CHECK_NEAR(toDouble(r), 1.0, 0, "FCOS 0");
    fromDouble(r, M_PI); fpCos(r, fpsr);
    CHECK_NEAR(toDouble(r), -1.0, 1e-8, "FCOS pi");
    fromDouble(r, 1.0); fpEtox(r, fpsr);
    CHECK_NEAR(toDouble(r), M_E, 1e-12, "FETOX 1");
    fromDouble(r, 0.0); fpEtox(r, fpsr);
    CHECK_NEAR(toDouble(r), 1.0, 0, "FETOX 0 = 1");
    fromDouble(r, M_E); fpLogn(r, fpsr);
    CHECK_NEAR(toDouble(r), 1.0, 1e-12, "FLOGN e");
    fromDouble(r, 1.0); fpLogn(r, fpsr);
    CHECK_NEAR(toDouble(r), 0.0, 1e-15, "FLOGN 1 = 0");
    fromDouble(r, 100.0); fpLog10(r, fpsr);
    CHECK_NEAR(toDouble(r), 2.0, 1e-12, "FLOG10 100");
    fromDouble(r, 1024.0); fpLog2(r, fpsr);
    CHECK_NEAR(toDouble(r), 10.0, 1e-12, "FLOG2 1024");
    fromDouble(r, 3.0); fpTwotox(r, fpsr);
    CHECK_NEAR(toDouble(r), 8.0, 1e-12, "FTWOTOX 3");
    fromDouble(r, 2.0); fpTentox(r, fpsr);
    CHECK_NEAR(toDouble(r), 100.0, 1e-10, "FTENTOX 2");
}

// ---- Rounding Precision Tests (68040 FSxxx/FDxxx) ----

void testRounding() {
    Registers::FPReg r;

    // fpRoundToSingle: 1/3 should lose precision
    fromDouble(r, 1.0 / 3.0);
    Registers::FPReg full = r;
    fpRoundToSingle(r);
    // After single rounding, converting back should match float(1/3)
    float f = 1.0f / 3.0f;
    u32 fbits; memcpy(&fbits, &f, 4);
    Registers::FPReg expected;
    fpFromSingle(expected, fbits);
    CHECK_FP(r, expected.exp, expected.mantissa, "fpRoundToSingle 1/3");

    // fpRoundToDouble: pi should match double precision
    fpMovecr(r, 0x00); // pi in extended
    fpRoundToDouble(r);
    double dpi = M_PI;
    u64 dpibits; memcpy(&dpibits, &dpi, 8);
    Registers::FPReg expPi;
    fpFromDouble(expPi, dpibits);
    CHECK_FP(r, expPi.exp, expPi.mantissa, "fpRoundToDouble pi");
}

// ---- Packed BCD Tests ----

void testPackedBCD() {
    Registers::FPReg r;

    // Pack: +1.0 with k=17 should give mantissa = 10000000000000000
    fromDouble(r, 1.0);
    u32 w0, w1, w2;
    fpToPacked(r, 17, w0, w1, w2);
    CHECK(!(w0 & 0x80000000), "packed +1.0 sign");
    // Integer digit should be 1
    CHECK((w0 & 0xF) != 0 || w1 != 0, "packed 1.0 has mantissa");

    // Unpack: zero
    fpFromPacked(r, 0, 0, 0);
    CHECK(fpIsZero(r), "unpack zero");

    // Unpack: +infinity
    fpFromPacked(r, 0x7FFF0000, 0, 0);
    // Infinity encoding: exponent field = 0xFFF
    Registers::FPReg inf;
    fpFromPacked(inf, 0x0FFF0000, 0, 0);
    CHECK(fpIsInf(inf), "unpack infinity");

    // Round-trip: 123.456
    fromDouble(r, 123.456);
    fpToPacked(r, 6, w0, w1, w2);
    Registers::FPReg r2;
    fpFromPacked(r2, w0, w1, w2);
    CHECK_NEAR(toDouble(r2), 123.456, 0.001, "packed round-trip 123.456");
}

// ---- Exception Triggering Tests ----

void testExceptions() {
    // DZ enabled: should return vector 50
    u32 fpsr = (u32)FPExc::DZ << FPSR::EXC_SHIFT;
    u32 fpcr = (u32)FPExc::DZ << FPCR::ENABLE_SHIFT;
    CHECK(fpCheckExceptions(fpsr, fpcr) == 50, "DZ exception vector 50");

    // OPERR enabled: should return vector 52
    fpsr = (u32)FPExc::OPERR << FPSR::EXC_SHIFT;
    fpcr = (u32)FPExc::OPERR << FPCR::ENABLE_SHIFT;
    CHECK(fpCheckExceptions(fpsr, fpcr) == 52, "OPERR exception vector 52");

    // OVFL enabled: should return vector 53
    fpsr = (u32)FPExc::OVFL << FPSR::EXC_SHIFT;
    fpcr = (u32)FPExc::OVFL << FPCR::ENABLE_SHIFT;
    CHECK(fpCheckExceptions(fpsr, fpcr) == 53, "OVFL exception vector 53");

    // No exception when not enabled
    fpsr = (u32)FPExc::DZ << FPSR::EXC_SHIFT;
    fpcr = 0;
    CHECK(fpCheckExceptions(fpsr, fpcr) == 0, "DZ not enabled = no exception");

    // SNAN: vector 54
    fpsr = (u32)FPExc::SNAN << FPSR::EXC_SHIFT;
    fpcr = (u32)FPExc::SNAN << FPCR::ENABLE_SHIFT;
    CHECK(fpCheckExceptions(fpsr, fpcr) == 54, "SNAN exception vector 54");
}

// ---- Condition Code Tests ----

void testConditionCodes() {
    Registers::FPReg r;
    u32 fpsr = 0;

    fromDouble(r, 42.0); fpSetCC(fpsr, r);
    CHECK(!(fpsr & FPSR::CC_N) && !(fpsr & FPSR::CC_Z) && !(fpsr & FPSR::CC_I) && !(fpsr & FPSR::CC_NAN), "CC positive");

    fpsr = 0; fpSetZero(r); fpSetCC(fpsr, r);
    CHECK((fpsr & FPSR::CC_Z) && !(fpsr & FPSR::CC_N), "CC +zero");

    fpsr = 0; fpSetZero(r, true); fpSetCC(fpsr, r);
    CHECK((fpsr & FPSR::CC_Z) && (fpsr & FPSR::CC_N), "CC -zero");

    fpsr = 0; fromDouble(r, -1.0); fpSetCC(fpsr, r);
    CHECK(fpsr & FPSR::CC_N, "CC negative");

    fpsr = 0; fpSetInf(r, false); fpSetCC(fpsr, r);
    CHECK(fpsr & FPSR::CC_I, "CC +inf");

    fpsr = 0; fpSetNaN(r); fpSetCC(fpsr, r);
    CHECK(fpsr & FPSR::CC_NAN, "CC NaN");
}

// ---- Cycle Count Tests ----

void testCycleCounts() {
    CHECK(fpCycleCount(0x22, false) == 28, "FADD 68881 = 28 cycles");
    CHECK(fpCycleCount(0x23, false) == 71, "FMUL 68881 = 71 cycles");
    CHECK(fpCycleCount(0x20, false) == 72, "FDIV 68881 = 72 cycles");
    CHECK(fpCycleCount(0x04, false) == 109, "FSQRT 68881 = 109 cycles");
    CHECK(fpCycleCount(0x0E, false) == 392, "FSIN 68881 = 392 cycles");
    CHECK(fpCycleCount(0x22, true) == 3, "FADD 68040 = 3 cycles");
    CHECK(fpCycleCount(0x20, true) == 38, "FDIV 68040 = 38 cycles");
}

// ---- Main ----

int main() {
    printf("=== Moira FPU Golden Vector Tests ===\n\n");

    testConversions();
    testConstants();
    testArithmetic();
    testTranscendental();
    testRounding();
    testPackedBCD();
    testExceptions();
    testConditionCodes();
    testCycleCounts();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
