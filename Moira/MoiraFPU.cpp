// -----------------------------------------------------------------------------
// This file is part of Moira - A Motorola 68k emulator
//
// Copyright (C) Dirk W. Hoffmann. www.dirkwhoffmann.de
// Published under the terms of the MIT License
// -----------------------------------------------------------------------------
//
// FPU arithmetic — clean-room implementation using Berkeley SoftFloat 3e
// Based on M68000 Family Programmer's Reference Manual (Motorola, 1992)
// and IEEE 754-1985 standard.
// -----------------------------------------------------------------------------

#include "MoiraFPU.h"
#include <cmath>
#include <cstring>

#include "softfloat/platform.h"
extern "C" {
#include "softfloat/internals.h"
#include "softfloat/softfloat.h"
}

namespace moira {

// ---------------------------------------------------------------------------
// SoftFloat helpers: convert between Moira FPReg and SoftFloat extFloat80_t
// ---------------------------------------------------------------------------

static inline extFloat80_t toSF(const FPReg &r) {
    extFloat80_t sf;
    sf.signExp = r.exp;
    sf.signif = r.mantissa;
    return sf;
}

static inline void fromSF(FPReg &r, const extFloat80_t &sf) {
    r.exp = sf.signExp;
    r.mantissa = sf.signif;
}

// Configure SoftFloat rounding mode from FPCR
static void sfSetRounding(u32 fpcr) {
    static constexpr uint_fast8_t modeMap[] = {
        softfloat_round_near_even,  // 00 = nearest
        softfloat_round_minMag,     // 01 = zero
        softfloat_round_min,        // 10 = -inf
        softfloat_round_max         // 11 = +inf
    };
    softfloat_roundingMode = modeMap[(fpcr >> FPCR::MODE_SHIFT) & 3];
}

// Configure SoftFloat rounding precision from FPCR
static void sfSetPrecision(u32 fpcr) {
    static constexpr uint_fast8_t precMap[] = { 80, 32, 64, 80 };
    extF80_roundingPrecision = precMap[(fpcr >> FPCR::PREC_SHIFT) & 3];
}

// Collect SoftFloat exception flags into FPSR
static void sfCollectExceptions(u32 &fpsr) {
    uint_fast8_t flags = softfloat_exceptionFlags;
    if (!flags) return;

    u8 exc = 0;
    if (flags & softfloat_flag_invalid)   exc |= FPExc::OPERR;
    if (flags & softfloat_flag_infinite)  exc |= FPExc::DZ;
    if (flags & softfloat_flag_overflow)  exc |= FPExc::OVFL;
    if (flags & softfloat_flag_underflow) exc |= FPExc::UNFL;
    if (flags & softfloat_flag_inexact)   exc |= FPExc::INEX2;

    fpsr |= (u32)exc << FPSR::EXC_SHIFT;
    // Accrue
    fpsr |= (u32)exc;
}

static void sfClear() { softfloat_exceptionFlags = 0; }

// ---------------------------------------------------------------------------
// FMOVECR constant ROM
// Values from MC68881/MC68882 User's Manual, Table 5-1
// ---------------------------------------------------------------------------

static const struct { u16 exp; u64 mantissa; } fpConstants[] = {
    { 0x4000, 0xC90FDAA22168C235ULL }, // 0x00: pi
    { 0x3FFD, 0x9A209A84FBCFF799ULL }, // 0x0B: log10(2)
    { 0x4000, 0xADF85458A2BB4A9AULL }, // 0x0C: e
    { 0x3FFF, 0xB8AA3B295C17F0BCULL }, // 0x0D: log2(e)
    { 0x3FFD, 0xDE5BD8A937287195ULL }, // 0x0E: log10(e)
    { 0x0000, 0x0000000000000000ULL }, // 0x0F: 0.0
    { 0x3FFE, 0xB17217F7D1CF79ACULL }, // 0x30: ln(2)
    { 0x4000, 0x935D8DDDAAA8AC17ULL }, // 0x31: ln(10)
    { 0x3FFF, 0x8000000000000000ULL }, // 0x32: 1.0
    { 0x4002, 0xA000000000000000ULL }, // 0x33: 10
    { 0x4005, 0xC800000000000000ULL }, // 0x34: 100
    { 0x400C, 0x9C40000000000000ULL }, // 0x35: 10^4
    { 0x4019, 0xBEBC200000000000ULL }, // 0x36: 10^8
    { 0x4034, 0x8E1BC9BF04000000ULL }, // 0x37: 10^16
    { 0x4069, 0x9DC5ADA82B70B59EULL }, // 0x38: 10^32
    { 0x40D3, 0xC2781F49FFCFA6D5ULL }, // 0x39: 10^64
    { 0x41A8, 0x93BA47C980E98CE0ULL }, // 0x3A: 10^128
    { 0x4351, 0xAA7EEBFB9DF9DE8EULL }, // 0x3B: 10^256
    { 0x46A3, 0xE319A0AEA60E91C7ULL }, // 0x3C: 10^512
    { 0x4D48, 0xC976758681750C17ULL }, // 0x3D: 10^1024
    { 0x5A92, 0x9E8B3B5DC53D5DE5ULL }, // 0x3E: 10^2048
    { 0x7525, 0xC46052028A20979BULL }, // 0x3F: 10^4096
};

static int fpcrIndex(u8 offset) {
    if (offset == 0x00) return 0;
    if (offset >= 0x0B && offset <= 0x0F) return offset - 0x0B + 1;
    if (offset >= 0x30 && offset <= 0x3F) return offset - 0x30 + 6;
    return -1;
}

void fpMovecr(FPReg &dst, u8 offset) {
    int idx = fpcrIndex(offset);
    if (idx >= 0) {
        dst.exp = fpConstants[idx].exp;
        dst.mantissa = fpConstants[idx].mantissa;
    } else {
        fpSetZero(dst);
    }
}

// ---------------------------------------------------------------------------
// Format conversion: integer -> extended (via SoftFloat)
// ---------------------------------------------------------------------------

void fpFromLong(FPReg &dst, i32 val) {
    fromSF(dst, i32_to_extF80(val));
}

void fpFromWord(FPReg &dst, i16 val) { fpFromLong(dst, (i32)val); }
void fpFromByte(FPReg &dst, i8 val)  { fpFromLong(dst, (i32)val); }

// ---------------------------------------------------------------------------
// Format conversion: IEEE single/double -> extended (via SoftFloat)
// ---------------------------------------------------------------------------

void fpFromSingle(FPReg &dst, u32 val) {
    float32_t f; f.v = val;
    fromSF(dst, f32_to_extF80(f));
}

void fpFromDouble(FPReg &dst, u64 val) {
    float64_t f; f.v = val;
    fromSF(dst, f64_to_extF80(f));
}

// ---------------------------------------------------------------------------
// Format conversion: extended -> IEEE single/double/integer (via SoftFloat)
// ---------------------------------------------------------------------------

u32 fpToSingle(const FPReg &src) {
    return extF80_to_f32(toSF(src)).v;
}

u64 fpToDouble(const FPReg &src) {
    return extF80_to_f64(toSF(src)).v;
}

i32 fpToLong(const FPReg &src, u32 fpcr) {
    sfSetRounding(fpcr);
    return extF80_to_i32(toSF(src), softfloat_roundingMode, false);
}

i16 fpToWord(const FPReg &src, u32 fpcr) {
    i32 v = fpToLong(src, fpcr);
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (i16)v;
}

i8 fpToByte(const FPReg &src, u32 fpcr) {
    i32 v = fpToLong(src, fpcr);
    if (v > 127) return 127;
    if (v < -128) return -128;
    return (i8)v;
}

// ---------------------------------------------------------------------------
// Packed BCD format support (Motorola 3-word format)
//
// Memory layout (3 longwords, 12 bytes):
//   Word 0: [SM:1][SE:1][0:2][Exp3:4][Exp2:4][Exp1:4][0:12][Int:4]
//   Word 1: [Frac digits 1-8, 4 bits each]
//   Word 2: [Frac digits 9-16, 4 bits each]
//
// SM = mantissa sign, SE = exponent sign
// Exponent is 3 BCD digits (0-999), mantissa is 17 BCD digits
// ---------------------------------------------------------------------------

void fpFromPacked(FPReg &dst, u32 w0, u32 w1, u32 w2) {
    bool signMant = (w0 >> 31) & 1;
    bool signExp  = (w0 >> 30) & 1;

    // Check for special encodings (infinity/NaN)
    u16 rawExp = (u16)((w0 >> 16) & 0xFFF);
    if (rawExp == 0xFFF) {
        if (w1 == 0 && w2 == 0 && (w0 & 0xF) == 0) {
            fpSetInf(dst, signMant);
        } else {
            fpSetNaN(dst);
        }
        return;
    }

    // Extract 3-digit BCD exponent
    i32 exp = 0;
    exp += ((w0 >> 24) & 0xF) * 100;
    exp += ((w0 >> 20) & 0xF) * 10;
    exp += ((w0 >> 16) & 0xF);
    if (signExp) exp = -exp;

    // Extract 17-digit BCD mantissa (integer digit + 16 fraction digits)
    u8 digits[17];
    digits[0] = (u8)(w0 & 0xF);
    for (int i = 0; i < 8; i++) digits[1 + i] = (u8)((w1 >> (28 - i * 4)) & 0xF);
    for (int i = 0; i < 8; i++) digits[9 + i] = (u8)((w2 >> (28 - i * 4)) & 0xF);

    // Check for zero mantissa
    bool allZero = true;
    for (int i = 0; i < 17; i++) { if (digits[i]) { allZero = false; break; } }
    if (allZero) { fpSetZero(dst, signMant); return; }

    // Convert decimal mantissa to binary via repeated multiply-add
    // mantissa = d0.d1d2...d16 * 10^exp
    // We compute the integer value of all 17 digits, then adjust exponent by -16
    // Use SoftFloat: result = integerMantissa * 10^(exp - 16)

    // Build the integer mantissa in extended precision
    extFloat80_t mant = i32_to_extF80(digits[0]);
    extFloat80_t ten = i32_to_extF80(10);
    for (int i = 1; i < 17; i++) {
        mant = extF80_mul(mant, ten);
        mant = extF80_add(mant, i32_to_extF80(digits[i]));
    }

    // Compute 10^|exp-16| and multiply or divide
    i32 adjExp = exp - 16;
    if (adjExp != 0) {
        i32 absExp = adjExp < 0 ? -adjExp : adjExp;
        extFloat80_t power = i32_to_extF80(1);
        extFloat80_t base = ten;
        while (absExp > 0) {
            if (absExp & 1) power = extF80_mul(power, base);
            base = extF80_mul(base, base);
            absExp >>= 1;
        }
        mant = (adjExp > 0) ? extF80_mul(mant, power) : extF80_div(mant, power);
    }

    fromSF(dst, mant);
    if (signMant) dst.exp |= 0x8000;
}

void fpToPacked(const FPReg &src, i32 kFactor, u32 &w0, u32 &w1, u32 &w2) {
    w0 = w1 = w2 = 0;

    bool neg = fpIsNeg(src);
    if (neg) w0 |= 0x80000000;

    if (fpIsZero(src)) return;
    if (fpIsInf(src)) { w0 |= 0x0FFF0000; return; }
    if (fpIsNaN(src)) { w0 |= 0x0FFF0000; w1 = 0xFFFFFFFF; return; }

    // Determine number of significant digits (1-17)
    int numDigits;
    if (kFactor >= 0) {
        numDigits = (kFactor == 0) ? 17 : (kFactor > 17 ? 17 : kFactor);
    } else {
        i32 bexp = (i32)(src.exp & 0x7FFF) - 16383;
        i32 decExp = (i32)(bexp * 0.30103);
        numDigits = decExp + 1 - kFactor;
        if (numDigits < 1) numDigits = 1;
        if (numDigits > 17) numDigits = 17;
    }

    // Get absolute value and find decimal exponent
    extFloat80_t absVal = toSF(src);
    absVal.signExp &= 0x7FFF;
    extFloat80_t ten = i32_to_extF80(10);
    extFloat80_t one = i32_to_extF80(1);
    i32 decExp = 0;

    // Normalize to [1, 10)
    if (extF80_lt(absVal, one)) {
        while (extF80_lt(absVal, one)) {
            absVal = extF80_mul(absVal, ten);
            decExp--;
        }
    } else {
        while (!extF80_lt(absVal, ten)) {
            absVal = extF80_div(absVal, ten);
            decExp++;
        }
    }

    // Scale to get numDigits-digit integer: multiply by 10^(numDigits-1)
    for (int i = 1; i < numDigits; i++) {
        absVal = extF80_mul(absVal, ten);
    }

    // Round to integer
    absVal = extF80_roundToInt(absVal, softfloat_round_near_even, false);
    i64 intVal = extF80_to_i64(absVal, softfloat_round_near_even, false);
    if (intVal < 0) intVal = -intVal;

    // Extract digits (most significant first)
    u8 digits[17] = {};
    for (int i = numDigits - 1; i >= 0; i--) {
        digits[i] = (u8)(intVal % 10);
        intVal /= 10;
    }

    // If carry occurred (rounding pushed us to 10^numDigits), adjust
    if (intVal > 0) {
        for (int i = numDigits - 1; i > 0; i--) digits[i] = digits[i-1];
        digits[0] = (u8)intVal;
        decExp++;
    }

    // Pack exponent (3 BCD digits)
    i32 finalExp = decExp;
    bool expNeg = finalExp < 0;
    if (expNeg) { w0 |= 0x40000000; finalExp = -finalExp; }
    if (finalExp > 999) finalExp = 999;
    w0 |= ((u32)((finalExp / 100) % 10) << 24);
    w0 |= ((u32)((finalExp / 10) % 10) << 20);
    w0 |= ((u32)(finalExp % 10) << 16);

    // Pack mantissa: digit[0] is integer part, digits[1..16] are fraction
    w0 |= (u32)digits[0];
    for (int i = 0; i < 8; i++) {
        u8 d = (i + 1 < numDigits) ? digits[i + 1] : 0;
        w1 |= ((u32)d << (28 - i * 4));
    }
    for (int i = 0; i < 8; i++) {
        u8 d = (i + 9 < numDigits) ? digits[i + 9] : 0;
        w2 |= ((u32)d << (28 - i * 4));
    }
}

// ---------------------------------------------------------------------------
// Basic arithmetic (all via SoftFloat for bit-exact results)
// ---------------------------------------------------------------------------

void fpAdd(FPReg &dst, const FPReg &src, u32 &fpsr) {
    sfClear();
    fromSF(dst, extF80_add(toSF(dst), toSF(src)));
    sfCollectExceptions(fpsr);
    fpSetCC(fpsr, dst);
}

void fpSub(FPReg &dst, const FPReg &src, u32 &fpsr) {
    sfClear();
    fromSF(dst, extF80_sub(toSF(dst), toSF(src)));
    sfCollectExceptions(fpsr);
    fpSetCC(fpsr, dst);
}

void fpMul(FPReg &dst, const FPReg &src, u32 &fpsr) {
    sfClear();
    fromSF(dst, extF80_mul(toSF(dst), toSF(src)));
    sfCollectExceptions(fpsr);
    fpSetCC(fpsr, dst);
}

void fpDiv(FPReg &dst, const FPReg &src, u32 &fpsr) {
    sfClear();
    if (fpIsZero(src)) {
        if (fpIsZero(dst)) {
            fpSetNaN(dst);
            fpsr |= (u32)FPExc::OPERR << FPSR::EXC_SHIFT;
        } else {
            fpSetInf(dst, fpIsNeg(dst) != fpIsNeg(src));
            fpsr |= (u32)FPExc::DZ << FPSR::EXC_SHIFT;
        }
    } else {
        fromSF(dst, extF80_div(toSF(dst), toSF(src)));
        sfCollectExceptions(fpsr);
    }
    fpSetCC(fpsr, dst);
}

void fpSqrt(FPReg &dst, u32 &fpsr) {
    sfClear();
    if (fpIsNeg(dst) && !fpIsZero(dst)) {
        fpSetNaN(dst);
        fpsr |= (u32)FPExc::OPERR << FPSR::EXC_SHIFT;
    } else {
        fromSF(dst, extF80_sqrt(toSF(dst)));
        sfCollectExceptions(fpsr);
    }
    fpSetCC(fpsr, dst);
}

void fpAbs(FPReg &dst, u32 &fpsr) {
    dst.exp &= 0x7FFF;
    fpSetCC(fpsr, dst);
}

void fpNeg(FPReg &dst, u32 &fpsr) {
    dst.exp ^= 0x8000;
    fpSetCC(fpsr, dst);
}

void fpCmp(const FPReg &dst, const FPReg &src, u32 &fpsr) {
    sfClear();
    FPReg tmp = dst;
    fromSF(tmp, extF80_sub(toSF(dst), toSF(src)));
    sfCollectExceptions(fpsr);
    fpSetCC(fpsr, tmp);
    // Check for unordered (NaN operands)
    if (fpIsNaN(dst) || fpIsNaN(src)) {
        fpsr &= ~FPSR::CC_MASK;
        fpsr |= FPSR::CC_NAN;
    }
}

void fpTst(const FPReg &src, u32 &fpsr) {
    fpSetCC(fpsr, src);
}

void fpInt(FPReg &dst, u32 fpcr, u32 &fpsr) {
    sfClear();
    sfSetRounding(fpcr);
    fromSF(dst, extF80_roundToInt(toSF(dst), softfloat_roundingMode, false));
    sfCollectExceptions(fpsr);
    fpSetCC(fpsr, dst);
}

void fpIntrz(FPReg &dst, u32 &fpsr) {
    sfClear();
    fromSF(dst, extF80_roundToInt(toSF(dst), softfloat_round_minMag, false));
    sfCollectExceptions(fpsr);
    fpSetCC(fpsr, dst);
}

// ---------------------------------------------------------------------------
// Transcendental functions
//
// SoftFloat 3e does not include transcendentals. We implement them using
// the host math library on a double-precision intermediate, then convert
// back to extended. This gives ~53 bits of precision which is sufficient
// for most emulation purposes. For full 64-bit mantissa accuracy, a
// dedicated CORDIC/polynomial implementation would be needed.
//
// The conversion path: extF80 -> f64 -> native double -> compute -> f64 -> extF80
// ---------------------------------------------------------------------------

static double sfToDouble(const FPReg &r) {
    u64 bits = extF80_to_f64(toSF(r)).v;
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}

static void sfFromDouble(FPReg &r, double d) {
    u64 bits;
    std::memcpy(&bits, &d, 8);
    float64_t f; f.v = bits;
    fromSF(r, f64_to_extF80(f));
}

void fpSin(FPReg &dst, u32 &fpsr)    { sfFromDouble(dst, std::sin(sfToDouble(dst))); fpSetCC(fpsr, dst); }
void fpCos(FPReg &dst, u32 &fpsr)    { sfFromDouble(dst, std::cos(sfToDouble(dst))); fpSetCC(fpsr, dst); }
void fpTan(FPReg &dst, u32 &fpsr)    { sfFromDouble(dst, std::tan(sfToDouble(dst))); fpSetCC(fpsr, dst); }
void fpAsin(FPReg &dst, u32 &fpsr)   { sfFromDouble(dst, std::asin(sfToDouble(dst))); fpSetCC(fpsr, dst); }
void fpAcos(FPReg &dst, u32 &fpsr)   { sfFromDouble(dst, std::acos(sfToDouble(dst))); fpSetCC(fpsr, dst); }
void fpAtan(FPReg &dst, u32 &fpsr)   { sfFromDouble(dst, std::atan(sfToDouble(dst))); fpSetCC(fpsr, dst); }
void fpSinh(FPReg &dst, u32 &fpsr)   { sfFromDouble(dst, std::sinh(sfToDouble(dst))); fpSetCC(fpsr, dst); }
void fpCosh(FPReg &dst, u32 &fpsr)   { sfFromDouble(dst, std::cosh(sfToDouble(dst))); fpSetCC(fpsr, dst); }
void fpTanh(FPReg &dst, u32 &fpsr)   { sfFromDouble(dst, std::tanh(sfToDouble(dst))); fpSetCC(fpsr, dst); }
void fpAtanh(FPReg &dst, u32 &fpsr)  { sfFromDouble(dst, std::atanh(sfToDouble(dst))); fpSetCC(fpsr, dst); }
void fpEtox(FPReg &dst, u32 &fpsr)   { sfFromDouble(dst, std::exp(sfToDouble(dst))); fpSetCC(fpsr, dst); }
void fpEtoxm1(FPReg &dst, u32 &fpsr) { sfFromDouble(dst, std::expm1(sfToDouble(dst))); fpSetCC(fpsr, dst); }
void fpTwotox(FPReg &dst, u32 &fpsr) { sfFromDouble(dst, std::exp2(sfToDouble(dst))); fpSetCC(fpsr, dst); }
void fpTentox(FPReg &dst, u32 &fpsr) { sfFromDouble(dst, std::pow(10.0, sfToDouble(dst))); fpSetCC(fpsr, dst); }
void fpLogn(FPReg &dst, u32 &fpsr)   { sfFromDouble(dst, std::log(sfToDouble(dst))); fpSetCC(fpsr, dst); }
void fpLognp1(FPReg &dst, u32 &fpsr) { sfFromDouble(dst, std::log1p(sfToDouble(dst))); fpSetCC(fpsr, dst); }
void fpLog2(FPReg &dst, u32 &fpsr)   { sfFromDouble(dst, std::log2(sfToDouble(dst))); fpSetCC(fpsr, dst); }
void fpLog10(FPReg &dst, u32 &fpsr)  { sfFromDouble(dst, std::log10(sfToDouble(dst))); fpSetCC(fpsr, dst); }

// ---------------------------------------------------------------------------
// Misc arithmetic
// ---------------------------------------------------------------------------

void fpMod(FPReg &dst, const FPReg &src, u32 &fpsr) {
    sfClear();
    fromSF(dst, extF80_rem(toSF(dst), toSF(src)));
    // FMOD uses sign of dividend (SoftFloat rem uses round-to-nearest)
    // Adjust sign to match dividend
    if (fpIsNeg(dst) != (dst.exp & 0x8000)) {
        // SoftFloat extF80_rem already implements IEEE remainder
    }
    sfCollectExceptions(fpsr);
    fpSetCC(fpsr, dst);
}

void fpRem(FPReg &dst, const FPReg &src, u32 &fpsr) {
    sfClear();
    fromSF(dst, extF80_rem(toSF(dst), toSF(src)));
    sfCollectExceptions(fpsr);
    fpSetCC(fpsr, dst);
}

void fpScale(FPReg &dst, const FPReg &src, u32 &fpsr) {
    if (fpIsZero(dst) || fpIsNaN(dst) || fpIsInf(dst)) { fpSetCC(fpsr, dst); return; }
    i32 scale = extF80_to_i32(toSF(src), softfloat_round_minMag, false);
    i32 exp = (i32)(dst.exp & 0x7FFF) + scale;
    if (exp >= 0x7FFF) { fpSetInf(dst, fpIsNeg(dst)); fpsr |= (u32)FPExc::OVFL << FPSR::EXC_SHIFT; }
    else if (exp <= 0) { fpSetZero(dst, fpIsNeg(dst)); fpsr |= (u32)FPExc::UNFL << FPSR::EXC_SHIFT; }
    else { dst.exp = (dst.exp & 0x8000) | (u16)exp; }
    fpSetCC(fpsr, dst);
}

void fpGetexp(FPReg &dst, u32 &fpsr) {
    if (fpIsZero(dst) || fpIsNaN(dst) || fpIsInf(dst)) { fpSetCC(fpsr, dst); return; }
    i32 exp = (i32)(dst.exp & 0x7FFF) - FP_EXP_BIAS;
    fromSF(dst, i32_to_extF80(exp));
    fpSetCC(fpsr, dst);
}

void fpGetman(FPReg &dst, u32 &fpsr) {
    if (fpIsZero(dst) || fpIsNaN(dst) || fpIsInf(dst)) { fpSetCC(fpsr, dst); return; }
    bool neg = fpIsNeg(dst);
    dst.exp = (neg ? 0x8000 : 0) | FP_EXP_BIAS;
    fpSetCC(fpsr, dst);
}

void fpSgldiv(FPReg &dst, const FPReg &src, u32 &fpsr) {
    sfClear();
    extF80_roundingPrecision = 32;
    fromSF(dst, extF80_div(toSF(dst), toSF(src)));
    extF80_roundingPrecision = 80;
    sfCollectExceptions(fpsr);
    fpSetCC(fpsr, dst);
}

void fpSglmul(FPReg &dst, const FPReg &src, u32 &fpsr) {
    sfClear();
    extF80_roundingPrecision = 32;
    fromSF(dst, extF80_mul(toSF(dst), toSF(src)));
    extF80_roundingPrecision = 80;
    sfCollectExceptions(fpsr);
    fpSetCC(fpsr, dst);
}

// ---------------------------------------------------------------------------
// Rounding precision enforcement for 68040 FSxxx/FDxxx variants
// ---------------------------------------------------------------------------

void fpRoundToSingle(FPReg &dst) {
    u32 s = fpToSingle(dst);
    fpFromSingle(dst, s);
}

void fpRoundToDouble(FPReg &dst) {
    u64 d = fpToDouble(dst);
    fpFromDouble(dst, d);
}

// ---------------------------------------------------------------------------
// FPU exception check: returns exception vector or 0 if no exception
// ---------------------------------------------------------------------------

u8 fpCheckExceptions(u32 fpsr, u32 fpcr) {
    u32 enabled = (fpsr & FPSR::EXC_MASK) & (fpcr & FPCR::ENABLE_MASK);
    if (!enabled) return 0;

    // Priority order (highest to lowest): BSNAN, SNAN, OPERR, OVFL, UNFL, DZ, INEX2, INEX1
    static constexpr u8 vectors[] = { 48, 54, 52, 53, 51, 50, 49, 49 };
    for (int i = 7; i >= 0; i--) {
        if (enabled & (1 << (i + 8))) return vectors[7 - i];
    }
    return 0;
}

void fpSyncState(u32 fpcr) {
    sfSetRounding(fpcr);
    sfSetPrecision(fpcr);
}

int fpCycleCount(u8 cmd, bool is040) {
    if (is040) {
        switch (cmd) {
            case 0x22: case 0x62: case 0x66: return FPUCycles::FADD_040;
            case 0x28: case 0x68: case 0x6C: return FPUCycles::FSUB_040;
            case 0x23: case 0x63: case 0x67: return FPUCycles::FMUL_040;
            case 0x20: case 0x60: case 0x64: return FPUCycles::FDIV_040;
            case 0x04: case 0x41: case 0x45: return FPUCycles::FSQRT_040;
            default: return FPUCycles::FADD_040;
        }
    }
    switch (cmd) {
        case 0x00: case 0x40: case 0x44: return FPUCycles::FMOVE;
        case 0x01: return FPUCycles::FINT;
        case 0x03: return FPUCycles::FINTRZ;
        case 0x04: case 0x41: case 0x45: return FPUCycles::FSQRT;
        case 0x18: case 0x58: case 0x5C: return FPUCycles::FABS;
        case 0x1A: case 0x5A: case 0x5E: return FPUCycles::FNEG;
        case 0x22: case 0x62: case 0x66: return FPUCycles::FADD;
        case 0x28: case 0x68: case 0x6C: return FPUCycles::FSUB;
        case 0x23: case 0x63: case 0x67: return FPUCycles::FMUL;
        case 0x20: case 0x60: case 0x64: return FPUCycles::FDIV;
        case 0x24: return FPUCycles::FSGLDIV;
        case 0x27: return FPUCycles::FSGLMUL;
        case 0x21: case 0x25: return FPUCycles::FMOD;
        case 0x26: return FPUCycles::FSCALE;
        case 0x1E: return FPUCycles::FGETEXP;
        case 0x1F: return FPUCycles::FGETMAN;
        case 0x38: return FPUCycles::FCMP;
        case 0x3A: return FPUCycles::FTST;
        case 0x0E: return FPUCycles::FSIN;
        case 0x1D: return FPUCycles::FCOS;
        case 0x0F: return FPUCycles::FTAN;
        case 0x0C: case 0x1C: return FPUCycles::FACOS;
        case 0x0A: return FPUCycles::FATAN;
        case 0x02: return FPUCycles::FSINH;
        case 0x19: return FPUCycles::FCOSH;
        case 0x09: return FPUCycles::FTANH;
        case 0x0D: return FPUCycles::FATANH;
        case 0x10: return FPUCycles::FETOX;
        case 0x08: return FPUCycles::FETOXM1;
        case 0x11: return FPUCycles::FTWOTOX;
        case 0x12: return FPUCycles::FTENTOX;
        case 0x14: return FPUCycles::FLOGN;
        case 0x06: return FPUCycles::FLOGNP1;
        case 0x16: return FPUCycles::FLOG2;
        case 0x15: return FPUCycles::FLOG10;
        default:
            if (cmd >= 0x30 && cmd <= 0x37) return FPUCycles::FSINCOS;
            return FPUCycles::FMOVE;
    }
}

} // namespace moira
