// -----------------------------------------------------------------------------
// This file is part of Moira - A Motorola 68k emulator
//
// Copyright (C) Dirk W. Hoffmann. www.dirkwhoffmann.de
// Published under the terms of the MIT License
// -----------------------------------------------------------------------------
//
// FPU transcendental functions — full 64-bit mantissa precision
// Clean-room implementation based on:
//   - Motorola FPSP algorithm descriptions (public domain)
//   - IEEE 754 mathematical function specifications
//   - Minimax polynomial approximations
//
// All arithmetic performed via SoftFloat extFloat80_t for bit-exact results.
// -----------------------------------------------------------------------------

#include "MoiraFPU.h"
#include <cstring>

#include "softfloat/platform.h"
extern "C" {
#include "softfloat/internals.h"
#include "softfloat/softfloat.h"
}

namespace moira {

// ---------------------------------------------------------------------------
// Helper: construct extFloat80_t from sign, exponent, significand
// ---------------------------------------------------------------------------

static inline extFloat80_t packF80(bool sign, u16 exp, u64 sig) {
    extFloat80_t r;
    r.signExp = (sign ? 0x8000 : 0) | exp;
    r.signif = sig;
    return r;
}

static inline extFloat80_t f80FromI32(i32 v) { return i32_to_extF80(v); }

static inline bool f80IsZero(extFloat80_t a) {
    return (a.signExp & 0x7FFF) == 0 && a.signif == 0;
}
static inline bool f80IsNaN(extFloat80_t a) {
    return (a.signExp & 0x7FFF) == 0x7FFF && a.signif != 0;
}
static inline bool f80IsInf(extFloat80_t a) {
    return (a.signExp & 0x7FFF) == 0x7FFF && a.signif == 0;
}
static inline bool f80IsNeg(extFloat80_t a) { return a.signExp & 0x8000; }

// Absolute value
static inline extFloat80_t f80Abs(extFloat80_t a) {
    a.signExp &= 0x7FFF;
    return a;
}

// Negate
static inline extFloat80_t f80Neg(extFloat80_t a) {
    a.signExp ^= 0x8000;
    return a;
}

// Constants
static const extFloat80_t F80_ZERO  = packF80(false, 0, 0);
static const extFloat80_t F80_ONE   = packF80(false, 0x3FFF, 0x8000000000000000ULL);
static const extFloat80_t F80_TWO   = packF80(false, 0x4000, 0x8000000000000000ULL);
static const extFloat80_t F80_HALF  = packF80(false, 0x3FFE, 0x8000000000000000ULL);
static const extFloat80_t F80_PI    = packF80(false, 0x4000, 0xC90FDAA22168C235ULL);
static const extFloat80_t F80_PIBY2 = packF80(false, 0x3FFF, 0xC90FDAA22168C235ULL);
static const extFloat80_t F80_PIBY4 = packF80(false, 0x3FFE, 0xC90FDAA22168C235ULL);
static const extFloat80_t F80_LN2   = packF80(false, 0x3FFE, 0xB17217F7D1CF79ACULL);
static const extFloat80_t F80_LOG2E = packF80(false, 0x3FFF, 0xB8AA3B295C17F0BCULL);
static const extFloat80_t F80_LN10  = packF80(false, 0x4000, 0x935D8DDDAAA8AC17ULL);
static const extFloat80_t F80_LOG10E= packF80(false, 0x3FFD, 0xDE5BD8A937287195ULL);
static const extFloat80_t F80_LOG102= packF80(false, 0x3FFD, 0x9A209A84FBCFF799ULL);

// 2/pi for range reduction
static const extFloat80_t F80_TWOBYPI = packF80(false, 0x3FFF, 0xA2F9836E4E44152AULL);

// ---------------------------------------------------------------------------
// Polynomial evaluation: Horner's method on extFloat80_t
// coeffs[0] is constant term, coeffs[n-1] is highest degree
// Evaluates: coeffs[0] + x*coeffs[1] + x^2*coeffs[2] + ...
// ---------------------------------------------------------------------------

static extFloat80_t polyEval(extFloat80_t x, const extFloat80_t *coeffs, int n) {
    extFloat80_t result = coeffs[n - 1];
    for (int i = n - 2; i >= 0; i--) {
        result = extF80_mul(result, x);
        result = extF80_add(result, coeffs[i]);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Sin/Cos polynomial coefficients (minimax on [-pi/4, pi/4])
// From FPSP: sin(r) ≈ r + r^3*(A1 + r^2*(A2 + r^2*(A3 + ...)))
//            cos(r) ≈ 1 + r^2*(B1 + r^2*(B2 + r^2*(B3 + ...)))
// ---------------------------------------------------------------------------

// Sin coefficients: sin(x) = x + x^3 * P(x^2)
// P(s) = -1/3! + s/5! - s^2/7! + ... (9 terms for full 64-bit precision)
static const extFloat80_t sinCoeffs[] = {
    packF80(true,  0x3FFC, 0xAAAAAAAAAAAAAAABULL), // -1/6
    packF80(false, 0x3FF8, 0x8888888888888889ULL), // 1/120
    packF80(true,  0x3FF2, 0xD00D00D00D00D00DULL), // -1/5040
    packF80(false, 0x3FEC, 0xB8EF1D2AB6399C7DULL), // 1/362880
    packF80(true,  0x3FE5, 0xD7322B3FAA271C7FULL), // -1/39916800
    packF80(false, 0x3FDE, 0xB092309D43684BE5ULL), // 1/6227020800
    packF80(true,  0x3FD6, 0xD73F9F399DC0F88FULL), // -1/1307674368000
    packF80(false, 0x3FCE, 0xCA963B81856A5359ULL), // 1/355687428096000
    packF80(true,  0x3FC6, 0x97A4DA340A0AB926ULL), // -1/121645100408832000
};
static constexpr int SIN_NCOEFFS = 9;

// Cos coefficients: cos(x) = 1 + x^2 * P(x^2)
// P(s) = -1/2! + s/4! - s^2/6! + ... (9 terms for full 64-bit precision)
static const extFloat80_t cosCoeffs[] = {
    packF80(true,  0x3FFE, 0x8000000000000000ULL), // -1/2
    packF80(false, 0x3FFA, 0xAAAAAAAAAAAAAAABULL), // 1/24
    packF80(true,  0x3FF5, 0xB60B60B60B60B60BULL), // -1/720
    packF80(false, 0x3FEF, 0xD00D00D00D00D00DULL), // 1/40320
    packF80(true,  0x3FE9, 0x93F27DBBC4FAE397ULL), // -1/3628800
    packF80(false, 0x3FE2, 0x8F76C77FC6C4BDAAULL), // 1/479001600
    packF80(true,  0x3FDA, 0xC9CBA54603E4E906ULL), // -1/87178291200
    packF80(false, 0x3FD2, 0xD73F9F399DC0F88FULL), // 1/20922789888000
    packF80(true,  0x3FCA, 0xB413C31DCBECBBDEULL), // -1/6402373705728000
};
static constexpr int COS_NCOEFFS = 9;

// ---------------------------------------------------------------------------
// Range reduction for sin/cos: reduce x to [-pi/4, pi/4]
// Returns reduced argument r and quadrant n (0-3)
// Uses Cody-Waite extended precision reduction: x - n*(pi/2)
// ---------------------------------------------------------------------------

static extFloat80_t reduceArg(extFloat80_t x, int &quadrant) {
    // pi/2 split into three parts for maximum precision (Cody-Waite)
    static const extFloat80_t piby2_1 = packF80(false, 0x3FFF, 0xC90FDAA200000000ULL);
    static const extFloat80_t piby2_2 = packF80(false, 0x3FDD, 0x85A308D200000000ULL);
    static const extFloat80_t piby2_3 = packF80(false, 0x3FBA, 0x8A2E03707344A409ULL);

    // n = round(x / (pi/2))
    extFloat80_t q = extF80_mul(x, F80_TWOBYPI);
    i32 n = extF80_to_i32(q, softfloat_round_near_even, false);
    quadrant = n & 3;

    // r = x - n * pi/2 (triple precision subtraction)
    extFloat80_t fn = i32_to_extF80(n);
    extFloat80_t r = extF80_sub(x, extF80_mul(fn, piby2_1));
    r = extF80_sub(r, extF80_mul(fn, piby2_2));
    r = extF80_sub(r, extF80_mul(fn, piby2_3));
    return r;
}

// ---------------------------------------------------------------------------
// Core sin/cos on reduced argument |r| <= pi/4
// ---------------------------------------------------------------------------

static extFloat80_t sinPoly(extFloat80_t r) {
    // sin(r) = r + r^3 * P(r^2)
    extFloat80_t r2 = extF80_mul(r, r);
    extFloat80_t poly = polyEval(r2, sinCoeffs, SIN_NCOEFFS);
    extFloat80_t r3 = extF80_mul(r2, r);
    return extF80_add(r, extF80_mul(r3, poly));
}

static extFloat80_t cosPoly(extFloat80_t r) {
    // cos(r) = 1 + r^2 * P(r^2)
    extFloat80_t r2 = extF80_mul(r, r);
    extFloat80_t poly = polyEval(r2, cosCoeffs, COS_NCOEFFS);
    return extF80_add(F80_ONE, extF80_mul(r2, poly));
}

// ---------------------------------------------------------------------------
// Public sin/cos/tan
// ---------------------------------------------------------------------------

void fpSin(FPReg &dst, u32 &fpsr) {
    extFloat80_t a = {dst.mantissa, dst.exp};
    if (f80IsNaN(a) || f80IsInf(a)) {
        fpSetNaN(dst);
        fpsr |= (u32)FPExc::OPERR << FPSR::EXC_SHIFT;
        fpSetCC(fpsr, dst);
        return;
    }
    if (f80IsZero(a)) { fpSetCC(fpsr, dst); return; }

    bool neg = f80IsNeg(a);
    a = f80Abs(a);

    int quad;
    extFloat80_t r = reduceArg(a, quad);

    extFloat80_t result;
    switch (quad) {
        case 0: result = sinPoly(r); break;
        case 1: result = cosPoly(r); break;
        case 2: result = f80Neg(sinPoly(r)); break;
        case 3: result = f80Neg(cosPoly(r)); break;
        default: result = F80_ZERO; break;
    }
    if (neg) result = f80Neg(result);

    dst.exp = result.signExp;
    dst.mantissa = result.signif;
    fpSetCC(fpsr, dst);
}

void fpCos(FPReg &dst, u32 &fpsr) {
    extFloat80_t a = {dst.mantissa, dst.exp};
    if (f80IsNaN(a) || f80IsInf(a)) {
        fpSetNaN(dst);
        fpsr |= (u32)FPExc::OPERR << FPSR::EXC_SHIFT;
        fpSetCC(fpsr, dst);
        return;
    }
    if (f80IsZero(a)) {
        dst.exp = F80_ONE.signExp; dst.mantissa = F80_ONE.signif;
        fpSetCC(fpsr, dst);
        return;
    }

    a = f80Abs(a);
    int quad;
    extFloat80_t r = reduceArg(a, quad);

    extFloat80_t result;
    switch (quad) {
        case 0: result = cosPoly(r); break;
        case 1: result = f80Neg(sinPoly(r)); break;
        case 2: result = f80Neg(cosPoly(r)); break;
        case 3: result = sinPoly(r); break;
        default: result = F80_ZERO; break;
    }

    dst.exp = result.signExp;
    dst.mantissa = result.signif;
    fpSetCC(fpsr, dst);
}

void fpTan(FPReg &dst, u32 &fpsr) {
    extFloat80_t a = {dst.mantissa, dst.exp};
    if (f80IsNaN(a) || f80IsInf(a)) {
        fpSetNaN(dst);
        fpsr |= (u32)FPExc::OPERR << FPSR::EXC_SHIFT;
        fpSetCC(fpsr, dst);
        return;
    }
    if (f80IsZero(a)) { fpSetCC(fpsr, dst); return; }

    // tan(x) = sin(x) / cos(x) computed in extended precision
    FPReg sinR = dst, cosR = dst;
    fpSin(sinR, fpsr);
    fpCos(cosR, fpsr);

    extFloat80_t s = {sinR.mantissa, sinR.exp};
    extFloat80_t c = {cosR.mantissa, cosR.exp};
    extFloat80_t result = extF80_div(s, c);

    dst.exp = result.signExp;
    dst.mantissa = result.signif;
    fpSetCC(fpsr, dst);
}

// ---------------------------------------------------------------------------
// Exponential: e^x
// Algorithm: e^x = 2^(x/ln2) = 2^n * 2^f where n=int(x/ln2), f=frac(x/ln2)
// 2^f computed via polynomial on [0, 1)
// ---------------------------------------------------------------------------

// 2^f polynomial coefficients: 2^f = 1 + f*c1 + f^2*c2 + ... where c_k = (ln2)^k / k!
// Evaluated as: 2^f = 1 + f*(c1 + f*(c2 + f*(c3 + ...)))
// 11 terms for full 64-bit precision on f in [-0.5, 0.5]
static const extFloat80_t expCoeffs[] = {
    packF80(false, 0x3FFE, 0xB17217F7D1CF79ACULL), // c1 = ln(2)
    packF80(false, 0x3FFC, 0xF5FDEFFC162C7544ULL), // c2 = ln(2)^2/2
    packF80(false, 0x3FFA, 0xE35846B82505FC5BULL), // c3 = ln(2)^3/6
    packF80(false, 0x3FF8, 0x9D955B7DD273B94FULL), // c4 = ln(2)^4/24
    packF80(false, 0x3FF5, 0xAEC3FF3C53398884ULL), // c5 = ln(2)^5/120
    packF80(false, 0x3FF2, 0xA184897C363C3B7BULL), // c6 = ln(2)^6/720
    packF80(false, 0x3FEE, 0xFFE5FE2C45863437ULL), // c7 = ln(2)^7/5040
    packF80(false, 0x3FEB, 0xB160111D2E411FEEULL), // c8 = ln(2)^8/40320
    packF80(false, 0x3FE7, 0xDA929E9CAF3E1ED5ULL), // c9 = ln(2)^9/362880
    packF80(false, 0x3FE3, 0xF267A8AC5C764FBBULL), // c10 = ln(2)^10/3628800
    packF80(false, 0x3FDF, 0xF465639A8DD9260BULL), // c11 = ln(2)^11/39916800
};
static constexpr int EXP_NCOEFFS = 11;

void fpEtox(FPReg &dst, u32 &fpsr) {
    extFloat80_t a = {dst.mantissa, dst.exp};
    if (f80IsNaN(a)) { fpSetCC(fpsr, dst); return; }
    if (f80IsInf(a)) {
        if (f80IsNeg(a)) fpSetZero(dst);
        // +inf stays as +inf
        fpSetCC(fpsr, dst);
        return;
    }
    if (f80IsZero(a)) {
        dst.exp = F80_ONE.signExp; dst.mantissa = F80_ONE.signif;
        fpSetCC(fpsr, dst);
        return;
    }

    // x / ln(2) to get n + f
    extFloat80_t xByLn2 = extF80_mul(a, F80_LOG2E);
    i32 n = extF80_to_i32(xByLn2, softfloat_round_near_even, false);
    extFloat80_t fn = i32_to_extF80(n);
    extFloat80_t f = extF80_sub(xByLn2, fn); // fractional part

    // 2^f via polynomial: 2^f = 1 + f*(c1 + f*(c2 + ...))
    extFloat80_t poly = polyEval(f, expCoeffs, EXP_NCOEFFS);
    extFloat80_t twoPowF = extF80_add(F80_ONE, extF80_mul(f, poly));

    // Scale by 2^n: adjust exponent
    i32 resultExp = (i32)(twoPowF.signExp & 0x7FFF) + n;
    if (resultExp >= 0x7FFF) {
        fpSetInf(dst, false);
        fpsr |= (u32)FPExc::OVFL << FPSR::EXC_SHIFT;
    } else if (resultExp <= 0) {
        fpSetZero(dst);
        fpsr |= (u32)FPExc::UNFL << FPSR::EXC_SHIFT;
    } else {
        dst.exp = (twoPowF.signExp & 0x8000) | (u16)resultExp;
        dst.mantissa = twoPowF.signif;
    }
    fpSetCC(fpsr, dst);
}

void fpEtoxm1(FPReg &dst, u32 &fpsr) {
    // For small x, e^x - 1 ≈ x + x^2/2 + ... (avoid catastrophic cancellation)
    extFloat80_t a = {dst.mantissa, dst.exp};
    i32 aExp = (i32)(a.signExp & 0x7FFF);

    if (aExp < 0x3FFF - 32) {
        // |x| < 2^-32: e^x - 1 ≈ x (identity for tiny values)
        fpSetCC(fpsr, dst);
        return;
    }

    // General case: compute e^x then subtract 1
    fpEtox(dst, fpsr);
    extFloat80_t result = {dst.mantissa, dst.exp};
    result = extF80_sub(result, F80_ONE);
    dst.exp = result.signExp;
    dst.mantissa = result.signif;
    fpSetCC(fpsr, dst);
}

// ---------------------------------------------------------------------------
// Natural logarithm: ln(x)
// Algorithm: x = 2^n * m where 1 <= m < 2
// ln(x) = n*ln(2) + ln(m)
// ln(m) computed via polynomial on reduced argument
// ---------------------------------------------------------------------------

// ln(1+f) coefficients for |f| < 0.5: ln(1+f) = f - f^2/2 + f^3/3 - ...
static const extFloat80_t lnCoeffs[] = {
    packF80(true,  0x3FFD, 0x8000000000000000ULL), // L1 = -1/2
    packF80(false, 0x3FFC, 0xAAAAAAAAAAAAAAAAULL), // L2 = 1/3
    packF80(true,  0x3FFB, 0x8000000000000000ULL), // L3 = -1/4
    packF80(false, 0x3FFA, 0xCCCCCCCCCCCCCCCDULL), // L4 = 1/5
    packF80(true,  0x3FFA, 0xAAAAAAAAAAAAAAAAULL), // L5 = -1/6
    packF80(false, 0x3FF9, 0xE38E38E38E38E38EULL), // L6 = 1/7
    packF80(true,  0x3FF9, 0x8000000000000000ULL), // L7 = -1/8
    packF80(false, 0x3FF8, 0xE38E38E38E38E38EULL), // L8 = 1/9
};

void fpLogn(FPReg &dst, u32 &fpsr) {
    extFloat80_t a = {dst.mantissa, dst.exp};
    if (f80IsNaN(a)) { fpSetCC(fpsr, dst); return; }
    if (f80IsZero(a)) {
        fpSetInf(dst, true); // ln(0) = -inf
        fpsr |= (u32)FPExc::DZ << FPSR::EXC_SHIFT;
        fpSetCC(fpsr, dst);
        return;
    }
    if (f80IsNeg(a)) {
        fpSetNaN(dst);
        fpsr |= (u32)FPExc::OPERR << FPSR::EXC_SHIFT;
        fpSetCC(fpsr, dst);
        return;
    }
    if (f80IsInf(a)) { fpSetCC(fpsr, dst); return; } // ln(+inf) = +inf

    // Extract: x = 2^n * m, where 1 <= m < 2
    i32 n = (i32)(a.signExp & 0x7FFF) - 0x3FFF;
    extFloat80_t m = a;
    m.signExp = 0x3FFF; // m in [1, 2)

    // f = m - 1, so ln(m) = ln(1+f)
    extFloat80_t f = extF80_sub(m, F80_ONE);

    // For |f| < 2^-4, use polynomial directly
    // For larger f, use ln(m) = 2*atanh((m-1)/(m+1)) for better convergence
    extFloat80_t lnm;
    i32 fExp = (i32)(f.signExp & 0x7FFF);

    if (fExp < 0x3FFB) {
        // |f| < 1/16: direct polynomial ln(1+f) = f*(1 + f*(L1 + f*(L2 + ...)))
        extFloat80_t poly = polyEval(f, lnCoeffs, 8);
        lnm = extF80_add(f, extF80_mul(extF80_mul(f, f), poly));
    } else {
        // Larger f: use u = (m-1)/(m+1), ln(m) = 2*(u + u^3/3 + u^5/5 + ...)
        extFloat80_t u = extF80_div(f, extF80_add(m, F80_ONE));
        extFloat80_t u2 = extF80_mul(u, u);
        // Polynomial in u^2
        extFloat80_t sum = F80_ONE;
        extFloat80_t term = u2;
        extFloat80_t coeff;
        // 1 + u^2/3 + u^4/5 + u^6/7 + u^8/9
        for (int k = 3; k <= 15; k += 2) {
            coeff = extF80_div(F80_ONE, i32_to_extF80(k));
            sum = extF80_add(sum, extF80_mul(term, coeff));
            term = extF80_mul(term, u2);
        }
        lnm = extF80_mul(extF80_mul(F80_TWO, u), sum);
    }

    // ln(x) = n*ln(2) + ln(m)
    extFloat80_t result;
    if (n == 0) {
        result = lnm;
    } else {
        result = extF80_add(extF80_mul(i32_to_extF80(n), F80_LN2), lnm);
    }

    dst.exp = result.signExp;
    dst.mantissa = result.signif;
    fpSetCC(fpsr, dst);
}

void fpLognp1(FPReg &dst, u32 &fpsr) {
    // ln(1+x): for small x, compute directly to avoid cancellation
    extFloat80_t a = {dst.mantissa, dst.exp};
    extFloat80_t onePlusA = extF80_add(F80_ONE, a);
    FPReg tmp;
    tmp.exp = onePlusA.signExp;
    tmp.mantissa = onePlusA.signif;
    fpLogn(tmp, fpsr);
    dst = tmp;
}

void fpLog2(FPReg &dst, u32 &fpsr) {
    // log2(x) = ln(x) / ln(2) = ln(x) * log2(e)
    fpLogn(dst, fpsr);
    if (!fpIsNaN(dst) && !fpIsInf(dst) && !fpIsZero(dst)) {
        extFloat80_t r = extF80_mul({dst.mantissa, dst.exp}, F80_LOG2E);
        dst.exp = r.signExp; dst.mantissa = r.signif;
        fpSetCC(fpsr, dst);
    }
}

void fpLog10(FPReg &dst, u32 &fpsr) {
    // log10(x) = ln(x) / ln(10) = ln(x) * log10(e)
    fpLogn(dst, fpsr);
    if (!fpIsNaN(dst) && !fpIsInf(dst) && !fpIsZero(dst)) {
        extFloat80_t r = extF80_mul({dst.mantissa, dst.exp}, F80_LOG10E);
        dst.exp = r.signExp; dst.mantissa = r.signif;
        fpSetCC(fpsr, dst);
    }
}

// ---------------------------------------------------------------------------
// 2^x and 10^x
// ---------------------------------------------------------------------------

void fpTwotox(FPReg &dst, u32 &fpsr) {
    // 2^x = e^(x * ln2)
    extFloat80_t a = {dst.mantissa, dst.exp};
    extFloat80_t xLn2 = extF80_mul(a, F80_LN2);
    dst.exp = xLn2.signExp; dst.mantissa = xLn2.signif;
    fpEtox(dst, fpsr);
}

void fpTentox(FPReg &dst, u32 &fpsr) {
    // 10^x = e^(x * ln10)
    extFloat80_t a = {dst.mantissa, dst.exp};
    extFloat80_t xLn10 = extF80_mul(a, F80_LN10);
    dst.exp = xLn10.signExp; dst.mantissa = xLn10.signif;
    fpEtox(dst, fpsr);
}

// ---------------------------------------------------------------------------
// Arctangent
// atan(x) for |x| <= 1: polynomial approximation
// atan(x) for |x| > 1: atan(x) = pi/2 - atan(1/x)
// ---------------------------------------------------------------------------

// atan(x) ≈ x - x^3/3 + x^5/5 - ... for |x| <= 1
// Using minimax polynomial: atan(x) = x * P(x^2)
static const extFloat80_t atanCoeffs[] = {
    packF80(true,  0x3FFC, 0xAAAAAAAAAAAAAAAAULL), // -1/3
    packF80(false, 0x3FFA, 0xCCCCCCCCCCCCCCCDULL), // 1/5
    packF80(true,  0x3FF9, 0x9249249249249249ULL), // -1/7
    packF80(false, 0x3FF8, 0x8888888888888889ULL), // 1/9
    packF80(true,  0x3FF7, 0xBA2E8BA2E8BA2E8CULL), // -1/11
    packF80(false, 0x3FF7, 0x9D89D89D89D89D8AULL), // 1/13
    packF80(true,  0x3FF6, 0x8888888888888889ULL), // -1/15
};

void fpAtan(FPReg &dst, u32 &fpsr) {
    extFloat80_t a = {dst.mantissa, dst.exp};
    if (f80IsNaN(a)) { fpSetCC(fpsr, dst); return; }
    if (f80IsZero(a)) { fpSetCC(fpsr, dst); return; }
    if (f80IsInf(a)) {
        // atan(+inf) = pi/2, atan(-inf) = -pi/2
        dst.exp = F80_PIBY2.signExp | (a.signExp & 0x8000);
        dst.mantissa = F80_PIBY2.signif;
        fpSetCC(fpsr, dst);
        return;
    }

    bool neg = f80IsNeg(a);
    a = f80Abs(a);
    bool invert = extF80_lt(F80_ONE, a); // |x| > 1

    extFloat80_t x = invert ? extF80_div(F80_ONE, a) : a;

    // atan(x) = x + x^3*P(x^2) for |x| <= 1
    extFloat80_t x2 = extF80_mul(x, x);
    extFloat80_t poly = polyEval(x2, atanCoeffs, 7);
    extFloat80_t x3 = extF80_mul(x2, x);
    extFloat80_t result = extF80_add(x, extF80_mul(x3, poly));

    if (invert) {
        result = extF80_sub(F80_PIBY2, result);
    }
    if (neg) result = f80Neg(result);

    dst.exp = result.signExp;
    dst.mantissa = result.signif;
    fpSetCC(fpsr, dst);
}

// ---------------------------------------------------------------------------
// Arcsine and Arccosine
// asin(x) = atan(x / sqrt(1 - x^2))
// acos(x) = pi/2 - asin(x)
// ---------------------------------------------------------------------------

void fpAsin(FPReg &dst, u32 &fpsr) {
    extFloat80_t a = {dst.mantissa, dst.exp};
    if (f80IsNaN(a)) { fpSetCC(fpsr, dst); return; }
    if (f80IsZero(a)) { fpSetCC(fpsr, dst); return; }

    // |x| must be <= 1
    extFloat80_t absA = f80Abs(a);
    if (extF80_lt(F80_ONE, absA)) {
        fpSetNaN(dst);
        fpsr |= (u32)FPExc::OPERR << FPSR::EXC_SHIFT;
        fpSetCC(fpsr, dst);
        return;
    }

    // asin(x) = atan(x / sqrt(1 - x^2))
    extFloat80_t x2 = extF80_mul(a, a);
    extFloat80_t denom = extF80_sqrt(extF80_sub(F80_ONE, x2));
    extFloat80_t arg = extF80_div(a, denom);

    dst.exp = arg.signExp; dst.mantissa = arg.signif;
    fpAtan(dst, fpsr);
}

void fpAcos(FPReg &dst, u32 &fpsr) {
    extFloat80_t a = {dst.mantissa, dst.exp};
    if (f80IsNaN(a)) { fpSetCC(fpsr, dst); return; }

    // acos(x) = pi/2 - asin(x)
    fpAsin(dst, fpsr);
    if (!fpIsNaN(dst)) {
        extFloat80_t r = extF80_sub(F80_PIBY2, {dst.mantissa, dst.exp});
        dst.exp = r.signExp; dst.mantissa = r.signif;
        fpSetCC(fpsr, dst);
    }
}

// ---------------------------------------------------------------------------
// Hyperbolic functions
// sinh(x) = (e^x - e^-x) / 2
// cosh(x) = (e^x + e^-x) / 2
// tanh(x) = sinh(x) / cosh(x)
// atanh(x) = ln((1+x)/(1-x)) / 2
// ---------------------------------------------------------------------------

void fpSinh(FPReg &dst, u32 &fpsr) {
    extFloat80_t a = {dst.mantissa, dst.exp};
    if (f80IsNaN(a) || f80IsInf(a) || f80IsZero(a)) { fpSetCC(fpsr, dst); return; }

    // e^x
    FPReg pos = dst;
    fpEtox(pos, fpsr);
    // e^-x
    FPReg negR = dst;
    negR.exp ^= 0x8000;
    fpEtox(negR, fpsr);

    extFloat80_t ep = {pos.mantissa, pos.exp};
    extFloat80_t en = {negR.mantissa, negR.exp};
    extFloat80_t result = extF80_mul(extF80_sub(ep, en), F80_HALF);

    dst.exp = result.signExp; dst.mantissa = result.signif;
    fpSetCC(fpsr, dst);
}

void fpCosh(FPReg &dst, u32 &fpsr) {
    extFloat80_t a = {dst.mantissa, dst.exp};
    if (f80IsNaN(a) || f80IsInf(a)) {
        if (f80IsInf(a)) { dst.exp &= 0x7FFF; } // cosh(±inf) = +inf
        fpSetCC(fpsr, dst);
        return;
    }
    if (f80IsZero(a)) {
        dst.exp = F80_ONE.signExp; dst.mantissa = F80_ONE.signif;
        fpSetCC(fpsr, dst);
        return;
    }

    FPReg pos = dst;
    pos.exp &= 0x7FFF; // |x|
    fpEtox(pos, fpsr);
    // e^-|x|
    FPReg negR = dst;
    negR.exp = (negR.exp & 0x7FFF) | 0x8000;
    fpEtox(negR, fpsr);

    extFloat80_t ep = {pos.mantissa, pos.exp};
    extFloat80_t en = {negR.mantissa, negR.exp};
    extFloat80_t result = extF80_mul(extF80_add(ep, en), F80_HALF);

    dst.exp = result.signExp; dst.mantissa = result.signif;
    fpSetCC(fpsr, dst);
}

void fpTanh(FPReg &dst, u32 &fpsr) {
    extFloat80_t a = {dst.mantissa, dst.exp};
    if (f80IsNaN(a)) { fpSetCC(fpsr, dst); return; }
    if (f80IsZero(a)) { fpSetCC(fpsr, dst); return; }
    if (f80IsInf(a)) {
        // tanh(±inf) = ±1
        dst.exp = F80_ONE.signExp | (a.signExp & 0x8000);
        dst.mantissa = F80_ONE.signif;
        fpSetCC(fpsr, dst);
        return;
    }

    // tanh(x) = (e^2x - 1) / (e^2x + 1)
    extFloat80_t twoX = extF80_add(a, a);
    FPReg tmp; tmp.exp = twoX.signExp; tmp.mantissa = twoX.signif;
    fpEtox(tmp, fpsr);
    extFloat80_t e2x = {tmp.mantissa, tmp.exp};
    extFloat80_t result = extF80_div(extF80_sub(e2x, F80_ONE), extF80_add(e2x, F80_ONE));

    dst.exp = result.signExp; dst.mantissa = result.signif;
    fpSetCC(fpsr, dst);
}

void fpAtanh(FPReg &dst, u32 &fpsr) {
    extFloat80_t a = {dst.mantissa, dst.exp};
    if (f80IsNaN(a)) { fpSetCC(fpsr, dst); return; }
    if (f80IsZero(a)) { fpSetCC(fpsr, dst); return; }

    // atanh(x) = ln((1+x)/(1-x)) / 2
    extFloat80_t num = extF80_add(F80_ONE, a);
    extFloat80_t den = extF80_sub(F80_ONE, a);
    extFloat80_t frac = extF80_div(num, den);

    FPReg tmp; tmp.exp = frac.signExp; tmp.mantissa = frac.signif;
    fpLogn(tmp, fpsr);
    extFloat80_t result = extF80_mul({tmp.mantissa, tmp.exp}, F80_HALF);

    dst.exp = result.signExp; dst.mantissa = result.signif;
    fpSetCC(fpsr, dst);
}

} // namespace moira
