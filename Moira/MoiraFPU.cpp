// -----------------------------------------------------------------------------
// This file is part of Moira - A Motorola 68k emulator
//
// Copyright (C) Dirk W. Hoffmann. www.dirkwhoffmann.de
// Published under the terms of the MIT License
// -----------------------------------------------------------------------------
//
// FPU arithmetic — clean-room implementation
// Based on M68000 Family Programmer's Reference Manual (Motorola, 1992)
// and IEEE 754-1985 standard.
// -----------------------------------------------------------------------------

#include "MoiraFPU.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace moira {

// ---------------------------------------------------------------------------
// FMOVECR constant ROM
// Values from MC68881/MC68882 User's Manual, Table 5-1
// Stored as { exp (with sign bit), mantissa }
// ---------------------------------------------------------------------------

static const struct { u16 exp; u64 mantissa; } fpConstants[] = {
    // 0x00: π
    { 0x4000, 0xC90FDAA22168C235ULL },
    // 0x0B: log10(2)
    { 0x3FFD, 0x9A209A84FBCFF799ULL },
    // 0x0C: e
    { 0x4000, 0xADF85458A2BB4A9AULL },
    // 0x0D: log2(e)
    { 0x3FFF, 0xB8AA3B295C17F0BCULL },
    // 0x0E: log10(e)
    { 0x3FFD, 0xDE5BD8A937287195ULL },
    // 0x0F: 0.0
    { 0x0000, 0x0000000000000000ULL },
    // 0x30: ln(2)
    { 0x3FFE, 0xB17217F7D1CF79ACULL },
    // 0x31: ln(10)
    { 0x4000, 0x935D8DDDAAA8AC17ULL },
    // 0x32: 10^0 = 1.0
    { 0x3FFF, 0x8000000000000000ULL },
    // 0x33: 10^1
    { 0x4002, 0xA000000000000000ULL },
    // 0x34: 10^2
    { 0x4005, 0xC800000000000000ULL },
    // 0x35: 10^4
    { 0x400C, 0x9C40000000000000ULL },
    // 0x36: 10^8
    { 0x4019, 0xBEBC200000000000ULL },
    // 0x37: 10^16
    { 0x4034, 0x8E1BC9BF04000000ULL },
    // 0x38: 10^32
    { 0x4069, 0x9DC5ADA82B70B59EULL },
    // 0x39: 10^64
    { 0x40D3, 0xC2781F49FFCFA6D5ULL },
    // 0x3A: 10^128
    { 0x41A8, 0x93BA47C980E98CE0ULL },
    // 0x3B: 10^256
    { 0x4351, 0xAA7EEBFB9DF9DE8EULL },
    // 0x3C: 10^512
    { 0x46A3, 0xE319A0AEA60E91C7ULL },
    // 0x3D: 10^1024
    { 0x4D48, 0xC976758681750C17ULL },
    // 0x3E: 10^2048
    { 0x5A92, 0x9E8B3B5DC53D5DE5ULL },
    // 0x3F: 10^4096
    { 0x7525, 0xC46052028A20979BULL },
};

// Map FMOVECR offset to table index
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
// Format conversion: integer → extended
// ---------------------------------------------------------------------------

void fpFromLong(FPReg &dst, i32 val) {
    if (val == 0) { fpSetZero(dst); return; }
    bool neg = val < 0;
    u64 abs = neg ? (u64)(-(i64)val) : (u64)val;
    int shift = 63 - __builtin_clzll(abs);
    dst.mantissa = abs << (63 - shift);
    dst.exp = (neg ? 0x8000 : 0) | (u16)(FP_EXP_BIAS + shift);
}

void fpFromWord(FPReg &dst, i16 val) { fpFromLong(dst, (i32)val); }
void fpFromByte(FPReg &dst, i8 val)  { fpFromLong(dst, (i32)val); }

// ---------------------------------------------------------------------------
// Format conversion: IEEE single/double → extended
// ---------------------------------------------------------------------------

void fpFromSingle(FPReg &dst, u32 val) {
    u16 sign = (val >> 31) ? 0x8000 : 0;
    i16 sexp = (i16)((val >> 23) & 0xFF);
    u64 mant = (u64)(val & 0x7FFFFF) << 40;

    if (sexp == 0 && mant == 0) {
        dst.exp = sign; dst.mantissa = 0; return;  // ±0
    }
    if (sexp == 0xFF) {
        dst.exp = sign | FP_EXP_MAX;
        dst.mantissa = mant ? (0x4000000000000000ULL | mant) : 0;
        return;  // ±∞ or NaN
    }
    if (sexp == 0) {
        // Denormal: normalize
        while (!(mant & 0x8000000000000000ULL)) { mant <<= 1; sexp--; }
        sexp++;
    } else {
        mant |= 0x8000000000000000ULL;  // Set explicit integer bit
    }
    dst.exp = sign | (u16)(sexp - 127 + FP_EXP_BIAS);
    dst.mantissa = mant;
}

void fpFromDouble(FPReg &dst, u64 val) {
    u16 sign = (val >> 63) ? 0x8000 : 0;
    i16 dexp = (i16)((val >> 52) & 0x7FF);
    u64 mant = (val & 0xFFFFFFFFFFFFFULL) << 11;

    if (dexp == 0 && mant == 0) {
        dst.exp = sign; dst.mantissa = 0; return;
    }
    if (dexp == 0x7FF) {
        dst.exp = sign | FP_EXP_MAX;
        dst.mantissa = mant ? (0x4000000000000000ULL | mant) : 0;
        return;
    }
    if (dexp == 0) {
        while (!(mant & 0x8000000000000000ULL)) { mant <<= 1; dexp--; }
        dexp++;
    } else {
        mant |= 0x8000000000000000ULL;
    }
    dst.exp = sign | (u16)(dexp - 1023 + FP_EXP_BIAS);
    dst.mantissa = mant;
}

// ---------------------------------------------------------------------------
// Format conversion: extended → IEEE single/double
// ---------------------------------------------------------------------------

u32 fpToSingle(const FPReg &src) {
    u32 sign = (src.exp & 0x8000) ? 0x80000000 : 0;
    i32 bexp = (i32)(src.exp & 0x7FFF) - FP_EXP_BIAS + 127;

    if (fpIsZero(src)) return sign;
    if (fpIsInf(src))  return sign | 0x7F800000;
    if (fpIsNaN(src))  return sign | 0x7FC00000;

    if (bexp >= 0xFF) return sign | 0x7F800000;  // Overflow → ∞
    if (bexp <= 0)    return sign;                 // Underflow → 0

    u32 mant = (u32)(src.mantissa >> 40) & 0x7FFFFF;
    return sign | ((u32)bexp << 23) | mant;
}

u64 fpToDouble(const FPReg &src) {
    u64 sign = (src.exp & 0x8000) ? 0x8000000000000000ULL : 0;
    i32 bexp = (i32)(src.exp & 0x7FFF) - FP_EXP_BIAS + 1023;

    if (fpIsZero(src)) return sign;
    if (fpIsInf(src))  return sign | 0x7FF0000000000000ULL;
    if (fpIsNaN(src))  return sign | 0x7FF8000000000000ULL;

    if (bexp >= 0x7FF) return sign | 0x7FF0000000000000ULL;
    if (bexp <= 0)     return sign;

    u64 mant = (src.mantissa >> 11) & 0xFFFFFFFFFFFFFULL;
    return sign | ((u64)bexp << 52) | mant;
}

// ---------------------------------------------------------------------------
// Format conversion: extended → integer
// ---------------------------------------------------------------------------

i32 fpToLong(const FPReg &src, u32 fpcr) {
    if (fpIsZero(src)) return 0;
    if (fpIsNaN(src) || fpIsInf(src)) return fpIsNeg(src) ? INT32_MIN : INT32_MAX;

    // Convert via double for simplicity (sufficient precision for i32)
    double d;
    u64 bits = fpToDouble(src);
    std::memcpy(&d, &bits, 8);

    // Apply rounding mode
    u8 mode = (fpcr >> 4) & 3;
    switch (mode) {
        case 0: return (i32)std::llround(d);        // Nearest
        case 1: return (i32)d;                       // Truncate
        case 2: return (i32)std::floor(d);           // -∞
        case 3: return (i32)std::ceil(d);            // +∞
    }
    return (i32)d;
}

i16 fpToWord(const FPReg &src, u32 fpcr) {
    i32 v = fpToLong(src, fpcr);
    if (v > INT16_MAX) return INT16_MAX;
    if (v < INT16_MIN) return INT16_MIN;
    return (i16)v;
}

i8 fpToByte(const FPReg &src, u32 fpcr) {
    i32 v = fpToLong(src, fpcr);
    if (v > INT8_MAX) return INT8_MAX;
    if (v < INT8_MIN) return INT8_MIN;
    return (i8)v;
}

// ---------------------------------------------------------------------------
// Internal: convert FPReg to/from native long double for arithmetic
// This is NOT a SoftFloat dependency — we use the host FPU for now.
// A proper SoftFloat backend can replace this later for bit-exact results.
// ---------------------------------------------------------------------------

static long double fpToNative(const FPReg &r) {
    if (fpIsZero(r)) return fpIsNeg(r) ? -0.0L : 0.0L;
    if (fpIsInf(r))  return fpIsNeg(r) ? -INFINITY : INFINITY;
    if (fpIsNaN(r))  return NAN;

    i32 exp = (i32)(r.exp & 0x7FFF) - FP_EXP_BIAS;
    long double val = std::ldexp((long double)r.mantissa, exp - 63);
    return fpIsNeg(r) ? -val : val;
}

static void fpFromNative(FPReg &dst, long double val) {
    if (val == 0.0L) { fpSetZero(dst, std::signbit(val)); return; }
    if (std::isinf(val)) { fpSetInf(dst, val < 0); return; }
    if (std::isnan(val)) { fpSetNaN(dst); return; }

    bool neg = val < 0;
    val = std::fabs(val);

    int exp;
    long double frac = std::frexp(val, &exp);
    // frexp returns [0.5, 1.0), we need [1.0, 2.0) with explicit integer bit
    u64 mant = (u64)(frac * (long double)(1ULL << 63)) << 1;
    if (mant == 0) mant = 0x8000000000000000ULL;

    dst.exp = (neg ? 0x8000 : 0) | (u16)(exp - 1 + FP_EXP_BIAS);
    dst.mantissa = mant;
}

// ---------------------------------------------------------------------------
// Basic arithmetic
// ---------------------------------------------------------------------------

void fpAdd(FPReg &dst, const FPReg &src, u32 &fpsr) {
    long double a = fpToNative(dst), b = fpToNative(src);
    fpFromNative(dst, a + b);
    fpSetCC(fpsr, dst);
}

void fpSub(FPReg &dst, const FPReg &src, u32 &fpsr) {
    long double a = fpToNative(dst), b = fpToNative(src);
    fpFromNative(dst, a - b);
    fpSetCC(fpsr, dst);
}

void fpMul(FPReg &dst, const FPReg &src, u32 &fpsr) {
    long double a = fpToNative(dst), b = fpToNative(src);
    fpFromNative(dst, a * b);
    fpSetCC(fpsr, dst);
}

void fpDiv(FPReg &dst, const FPReg &src, u32 &fpsr) {
    if (fpIsZero(src)) {
        if (fpIsZero(dst)) { fpSetNaN(dst); fpsr |= (u32)FPExc::OPERR << FPSR::EXC_SHIFT; }
        else { fpSetInf(dst, fpIsNeg(dst) != fpIsNeg(src)); fpsr |= (u32)FPExc::DZ << FPSR::EXC_SHIFT; }
        fpSetCC(fpsr, dst);
        return;
    }
    long double a = fpToNative(dst), b = fpToNative(src);
    fpFromNative(dst, a / b);
    fpSetCC(fpsr, dst);
}

void fpSqrt(FPReg &dst, u32 &fpsr) {
    if (fpIsNeg(dst) && !fpIsZero(dst)) {
        fpSetNaN(dst); fpsr |= (u32)FPExc::OPERR << FPSR::EXC_SHIFT;
    } else {
        fpFromNative(dst, std::sqrt(fpToNative(dst)));
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
    // CMP computes dst - src and sets condition codes (result is discarded)
    FPReg tmp = dst;
    fpSub(tmp, src, fpsr);
    // fpSub already set CC
}

void fpTst(const FPReg &src, u32 &fpsr) {
    fpSetCC(fpsr, src);
}

void fpInt(FPReg &dst, u32 fpcr, u32 &fpsr) {
    long double v = fpToNative(dst);
    u8 mode = (fpcr >> 4) & 3;
    switch (mode) {
        case 0: v = std::round(v); break;
        case 1: v = std::trunc(v); break;
        case 2: v = std::floor(v); break;
        case 3: v = std::ceil(v);  break;
    }
    fpFromNative(dst, v);
    fpSetCC(fpsr, dst);
}

void fpIntrz(FPReg &dst, u32 &fpsr) {
    fpFromNative(dst, std::trunc(fpToNative(dst)));
    fpSetCC(fpsr, dst);
}

// ---------------------------------------------------------------------------
// Transcendental functions
// ---------------------------------------------------------------------------

void fpSin(FPReg &dst, u32 &fpsr)    { fpFromNative(dst, std::sin(fpToNative(dst))); fpSetCC(fpsr, dst); }
void fpCos(FPReg &dst, u32 &fpsr)    { fpFromNative(dst, std::cos(fpToNative(dst))); fpSetCC(fpsr, dst); }
void fpTan(FPReg &dst, u32 &fpsr)    { fpFromNative(dst, std::tan(fpToNative(dst))); fpSetCC(fpsr, dst); }
void fpAsin(FPReg &dst, u32 &fpsr)   { fpFromNative(dst, std::asin(fpToNative(dst))); fpSetCC(fpsr, dst); }
void fpAcos(FPReg &dst, u32 &fpsr)   { fpFromNative(dst, std::acos(fpToNative(dst))); fpSetCC(fpsr, dst); }
void fpAtan(FPReg &dst, u32 &fpsr)   { fpFromNative(dst, std::atan(fpToNative(dst))); fpSetCC(fpsr, dst); }
void fpSinh(FPReg &dst, u32 &fpsr)   { fpFromNative(dst, std::sinh(fpToNative(dst))); fpSetCC(fpsr, dst); }
void fpCosh(FPReg &dst, u32 &fpsr)   { fpFromNative(dst, std::cosh(fpToNative(dst))); fpSetCC(fpsr, dst); }
void fpTanh(FPReg &dst, u32 &fpsr)   { fpFromNative(dst, std::tanh(fpToNative(dst))); fpSetCC(fpsr, dst); }
void fpAtanh(FPReg &dst, u32 &fpsr)  { fpFromNative(dst, std::atanh(fpToNative(dst))); fpSetCC(fpsr, dst); }
void fpEtox(FPReg &dst, u32 &fpsr)   { fpFromNative(dst, std::exp(fpToNative(dst))); fpSetCC(fpsr, dst); }
void fpEtoxm1(FPReg &dst, u32 &fpsr) { fpFromNative(dst, std::expm1(fpToNative(dst))); fpSetCC(fpsr, dst); }
void fpTwotox(FPReg &dst, u32 &fpsr) { fpFromNative(dst, std::exp2(fpToNative(dst))); fpSetCC(fpsr, dst); }
void fpTentox(FPReg &dst, u32 &fpsr) { fpFromNative(dst, std::pow(10.0L, fpToNative(dst))); fpSetCC(fpsr, dst); }
void fpLogn(FPReg &dst, u32 &fpsr)   { fpFromNative(dst, std::log(fpToNative(dst))); fpSetCC(fpsr, dst); }
void fpLognp1(FPReg &dst, u32 &fpsr) { fpFromNative(dst, std::log1p(fpToNative(dst))); fpSetCC(fpsr, dst); }
void fpLog2(FPReg &dst, u32 &fpsr)   { fpFromNative(dst, std::log2(fpToNative(dst))); fpSetCC(fpsr, dst); }
void fpLog10(FPReg &dst, u32 &fpsr)  { fpFromNative(dst, std::log10(fpToNative(dst))); fpSetCC(fpsr, dst); }

// ---------------------------------------------------------------------------
// Misc arithmetic
// ---------------------------------------------------------------------------

void fpMod(FPReg &dst, const FPReg &src, u32 &fpsr) {
    long double a = fpToNative(dst), b = fpToNative(src);
    fpFromNative(dst, std::fmod(a, b));
    fpSetCC(fpsr, dst);
}

void fpRem(FPReg &dst, const FPReg &src, u32 &fpsr) {
    long double a = fpToNative(dst), b = fpToNative(src);
    fpFromNative(dst, std::remainder(a, b));
    fpSetCC(fpsr, dst);
}

void fpScale(FPReg &dst, const FPReg &src, u32 &fpsr) {
    i32 scale = fpToLong(src, 0);  // Truncate
    long double v = std::ldexp(fpToNative(dst), scale);
    fpFromNative(dst, v);
    fpSetCC(fpsr, dst);
}

void fpGetexp(FPReg &dst, u32 &fpsr) {
    if (fpIsZero(dst) || fpIsNaN(dst) || fpIsInf(dst)) { fpSetCC(fpsr, dst); return; }
    i32 exp = (i32)(dst.exp & 0x7FFF) - FP_EXP_BIAS;
    fpFromLong(dst, exp);
    fpSetCC(fpsr, dst);
}

void fpGetman(FPReg &dst, u32 &fpsr) {
    if (fpIsZero(dst) || fpIsNaN(dst) || fpIsInf(dst)) { fpSetCC(fpsr, dst); return; }
    // Return mantissa with exponent = 0 (i.e., value in [1.0, 2.0))
    bool neg = fpIsNeg(dst);
    dst.exp = (neg ? 0x8000 : 0) | FP_EXP_BIAS;
    fpSetCC(fpsr, dst);
}

void fpSgldiv(FPReg &dst, const FPReg &src, u32 &fpsr) {
    fpDiv(dst, src, fpsr);
    // Round to single precision
    u32 s = fpToSingle(dst);
    fpFromSingle(dst, s);
    fpSetCC(fpsr, dst);
}

void fpSglmul(FPReg &dst, const FPReg &src, u32 &fpsr) {
    fpMul(dst, src, fpsr);
    u32 s = fpToSingle(dst);
    fpFromSingle(dst, s);
    fpSetCC(fpsr, dst);
}

} // namespace moira
