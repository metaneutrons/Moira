// FPU Unit Tests — golden vector tests
// Values verified against IEEE 754 and Motorola MC68881 documentation

#include "MoiraFPU.h"
#include <cstdio>
#include <cmath>
#include <cassert>

using namespace moira;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { tests_passed++; } \
    else { tests_failed++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

#define CHECK_NEAR(a, b, eps, msg) do { \
    double _a = (a), _b = (b), _e = (eps); \
    if (std::fabs(_a - _b) <= _e) { tests_passed++; } \
    else { tests_failed++; printf("FAIL: %s: got %.15g, expected %.15g (line %d)\n", msg, _a, _b, __LINE__); } \
} while(0)

// Helper: convert FPReg to double for comparison
static double toDouble(const Registers::FPReg &r) {
    u64 bits = fpToDouble(r);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static void fromDouble(Registers::FPReg &r, double d) {
    u64 bits;
    memcpy(&bits, &d, 8);
    fpFromDouble(r, bits);
}

// ---- Format Conversion Tests ----

void testConversions() {
    Registers::FPReg r;

    // Integer → Extended → Integer round-trip
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

    // Single → Extended → Single round-trip
    float f = 3.14f;
    u32 fbits;
    memcpy(&fbits, &f, 4);
    fpFromSingle(r, fbits);
    u32 back = fpToSingle(r);
    float fback;
    memcpy(&fback, &back, 4);
    CHECK_NEAR(fback, 3.14f, 1e-6, "single round-trip 3.14");

    // Double → Extended → Double round-trip
    fromDouble(r, 2.718281828459045);
    CHECK_NEAR(toDouble(r), 2.718281828459045, 1e-12, "double round-trip e");

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

// ---- FMOVECR Constants ----

void testConstants() {
    Registers::FPReg r;

    fpMovecr(r, 0x00); // π
    CHECK_NEAR(toDouble(r), M_PI, 1e-15, "FMOVECR pi");

    fpMovecr(r, 0x0C); // e
    CHECK_NEAR(toDouble(r), M_E, 1e-15, "FMOVECR e");

    fpMovecr(r, 0x30); // ln(2)
    CHECK_NEAR(toDouble(r), M_LN2, 1e-15, "FMOVECR ln2");

    fpMovecr(r, 0x31); // ln(10)
    CHECK_NEAR(toDouble(r), M_LN10, 1e-15, "FMOVECR ln10");

    fpMovecr(r, 0x0D); // log2(e)
    CHECK_NEAR(toDouble(r), M_LOG2E, 1e-15, "FMOVECR log2e");

    fpMovecr(r, 0x0E); // log10(e)
    CHECK_NEAR(toDouble(r), M_LOG10E, 1e-15, "FMOVECR log10e");

    fpMovecr(r, 0x0F); // 0.0
    CHECK(fpIsZero(r), "FMOVECR zero");

    fpMovecr(r, 0x32); // 1.0
    CHECK_NEAR(toDouble(r), 1.0, 1e-15, "FMOVECR 1.0");

    fpMovecr(r, 0x33); // 10.0
    CHECK_NEAR(toDouble(r), 10.0, 1e-15, "FMOVECR 10.0");

    fpMovecr(r, 0x34); // 100.0
    CHECK_NEAR(toDouble(r), 100.0, 1e-15, "FMOVECR 100.0");

    fpMovecr(r, 0x35); // 10000.0
    CHECK_NEAR(toDouble(r), 10000.0, 1e-10, "FMOVECR 10^4");
}

// ---- Basic Arithmetic ----

void testArithmetic() {
    Registers::FPReg a, b;
    u32 fpsr = 0;

    // ADD: 1.5 + 2.5 = 4.0
    fromDouble(a, 1.5);
    fromDouble(b, 2.5);
    fpAdd(a, b, fpsr);
    CHECK_NEAR(toDouble(a), 4.0, 1e-15, "FADD 1.5+2.5");

    // SUB: 10.0 - 3.0 = 7.0
    fromDouble(a, 10.0);
    fromDouble(b, 3.0);
    fpSub(a, b, fpsr);
    CHECK_NEAR(toDouble(a), 7.0, 1e-15, "FSUB 10-3");

    // MUL: 6.0 * 7.0 = 42.0
    fromDouble(a, 6.0);
    fromDouble(b, 7.0);
    fpMul(a, b, fpsr);
    CHECK_NEAR(toDouble(a), 42.0, 1e-15, "FMUL 6*7");

    // DIV: 22.0 / 7.0 ≈ 3.142857...
    fromDouble(a, 22.0);
    fromDouble(b, 7.0);
    fpDiv(a, b, fpsr);
    CHECK_NEAR(toDouble(a), 22.0/7.0, 1e-12, "FDIV 22/7");

    // DIV by zero
    fpsr = 0;
    fromDouble(a, 1.0);
    fpSetZero(b);
    fpDiv(a, b, fpsr);
    CHECK(fpIsInf(a), "FDIV 1/0 = inf");
    CHECK((fpsr >> 8) & FPExc::DZ, "FDIV 1/0 sets DZ");

    // 0/0 = NaN
    fpsr = 0;
    fpSetZero(a);
    fpSetZero(b);
    fpDiv(a, b, fpsr);
    CHECK(fpIsNaN(a), "FDIV 0/0 = NaN");
    CHECK((fpsr >> 8) & FPExc::OPERR, "FDIV 0/0 sets OPERR");

    // SQRT(4.0) = 2.0
    fromDouble(a, 4.0);
    fpSqrt(a, fpsr);
    CHECK_NEAR(toDouble(a), 2.0, 1e-15, "FSQRT 4");

    // SQRT(-1) = NaN
    fpsr = 0;
    fromDouble(a, -1.0);
    fpSqrt(a, fpsr);
    CHECK(fpIsNaN(a), "FSQRT -1 = NaN");

    // ABS(-42.0) = 42.0
    fromDouble(a, -42.0);
    fpAbs(a, fpsr);
    CHECK_NEAR(toDouble(a), 42.0, 1e-15, "FABS -42");

    // NEG(42.0) = -42.0
    fromDouble(a, 42.0);
    fpNeg(a, fpsr);
    CHECK_NEAR(toDouble(a), -42.0, 1e-15, "FNEG 42");

    // CMP: sets condition codes
    fpsr = 0;
    fromDouble(a, 5.0);
    fromDouble(b, 3.0);
    fpCmp(a, b, fpsr);
    CHECK(!(fpsr & FPSR::CC_N), "FCMP 5>3: not negative");
    CHECK(!(fpsr & FPSR::CC_Z), "FCMP 5>3: not zero");

    fpsr = 0;
    fromDouble(a, 3.0);
    fromDouble(b, 3.0);
    fpCmp(a, b, fpsr);
    CHECK(fpsr & FPSR::CC_Z, "FCMP 3==3: zero");

    fpsr = 0;
    fromDouble(a, 1.0);
    fromDouble(b, 5.0);
    fpCmp(a, b, fpsr);
    CHECK(fpsr & FPSR::CC_N, "FCMP 1<5: negative");
}

// ---- Transcendental Functions ----

void testTranscendental() {
    Registers::FPReg r;
    u32 fpsr = 0;

    fromDouble(r, 0.0);
    fpSin(r, fpsr);
    CHECK_NEAR(toDouble(r), 0.0, 1e-15, "FSIN 0");

    fromDouble(r, M_PI / 2.0);
    fpSin(r, fpsr);
    CHECK_NEAR(toDouble(r), 1.0, 1e-12, "FSIN pi/2");

    fromDouble(r, 0.0);
    fpCos(r, fpsr);
    CHECK_NEAR(toDouble(r), 1.0, 1e-15, "FCOS 0");

    fromDouble(r, M_PI);
    fpCos(r, fpsr);
    CHECK_NEAR(toDouble(r), -1.0, 1e-12, "FCOS pi");

    fromDouble(r, 1.0);
    fpEtox(r, fpsr);
    CHECK_NEAR(toDouble(r), M_E, 1e-12, "FETOX 1 = e");

    fromDouble(r, M_E);
    fpLogn(r, fpsr);
    CHECK_NEAR(toDouble(r), 1.0, 1e-12, "FLOGN e = 1");

    fromDouble(r, 100.0);
    fpLog10(r, fpsr);
    CHECK_NEAR(toDouble(r), 2.0, 1e-12, "FLOG10 100 = 2");

    fromDouble(r, 1024.0);
    fpLog2(r, fpsr);
    CHECK_NEAR(toDouble(r), 10.0, 1e-12, "FLOG2 1024 = 10");

    fromDouble(r, 3.0);
    fpTwotox(r, fpsr);
    CHECK_NEAR(toDouble(r), 8.0, 1e-12, "FTWOTOX 3 = 8");

    fromDouble(r, 2.0);
    fpTentox(r, fpsr);
    CHECK_NEAR(toDouble(r), 100.0, 1e-10, "FTENTOX 2 = 100");
}

// ---- Condition Code Tests ----

void testConditionCodes() {
    Registers::FPReg r;
    u32 fpsr = 0;

    // Positive number
    fromDouble(r, 42.0);
    fpSetCC(fpsr, r);
    CHECK(!(fpsr & FPSR::CC_N), "CC: positive not N");
    CHECK(!(fpsr & FPSR::CC_Z), "CC: positive not Z");
    CHECK(!(fpsr & FPSR::CC_I), "CC: positive not I");
    CHECK(!(fpsr & FPSR::CC_NAN), "CC: positive not NAN");

    // Zero
    fpsr = 0;
    fpSetZero(r);
    fpSetCC(fpsr, r);
    CHECK(fpsr & FPSR::CC_Z, "CC: zero is Z");

    // Negative
    fpsr = 0;
    fromDouble(r, -1.0);
    fpSetCC(fpsr, r);
    CHECK(fpsr & FPSR::CC_N, "CC: negative is N");

    // Infinity
    fpsr = 0;
    fpSetInf(r, false);
    fpSetCC(fpsr, r);
    CHECK(fpsr & FPSR::CC_I, "CC: +inf is I");

    // NaN
    fpsr = 0;
    fpSetNaN(r);
    fpSetCC(fpsr, r);
    CHECK(fpsr & FPSR::CC_NAN, "CC: NaN is NAN");
}

int main() {
    printf("=== Moira FPU Unit Tests ===\n\n");

    testConversions();
    testConstants();
    testArithmetic();
    testTranscendental();
    testConditionCodes();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
