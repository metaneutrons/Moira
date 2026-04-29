// -----------------------------------------------------------------------------
// This file is part of Moira - A Motorola 68k emulator
//
// Copyright (C) Dirk W. Hoffmann. www.dirkwhoffmann.de
// Published under the terms of the MIT License
// -----------------------------------------------------------------------------
//
// FPU support added by Fabian Schmieder (metaneutrons)
// Clean-room implementation based on:
//   - M68000 Family Programmer's Reference Manual (Motorola, 1992)
//   - MC68881/MC68882 User's Manual (Motorola, 1987)
//   - IEEE 754-1985
// -----------------------------------------------------------------------------

#pragma once

#include "MoiraTypes.h"

namespace moira {

// ---------------------------------------------------------------------------
// Extended Precision value (80-bit: 1 sign + 15 exponent + 64 mantissa)
// Stored as { u16 exp, u64 mantissa } matching Registers::fp[n]
// ---------------------------------------------------------------------------

using FPReg = Registers::FPReg;

// Exponent bias for 80-bit extended precision
static constexpr u16 FP_EXP_BIAS = 16383;
static constexpr u16 FP_EXP_MAX  = 0x7FFF;

// ---------------------------------------------------------------------------
// FPCR bit fields
// ---------------------------------------------------------------------------

namespace FPCR {
    // Rounding precision (bits 7-6)
    static constexpr u32 PREC_MASK  = 0xC0;
    static constexpr int PREC_SHIFT = 6;
    // Rounding mode (bits 5-4)
    static constexpr u32 MODE_MASK  = 0x30;
    static constexpr int MODE_SHIFT = 4;
    // Exception enable (bits 15-8)
    static constexpr u32 ENABLE_MASK = 0xFF00;
    static constexpr int ENABLE_SHIFT = 8;
}

// ---------------------------------------------------------------------------
// FPSR bit fields
// ---------------------------------------------------------------------------

namespace FPSR {
    // Condition codes (bits 27-24)
    static constexpr u32 CC_N   = 1 << 27;
    static constexpr u32 CC_Z   = 1 << 26;
    static constexpr u32 CC_I   = 1 << 25;
    static constexpr u32 CC_NAN = 1 << 24;
    static constexpr u32 CC_MASK = 0x0F000000;

    // Quotient (bits 23-16)
    static constexpr u32 QUOT_MASK = 0x00FF0000;

    // Exception status (bits 15-8)
    static constexpr u32 EXC_MASK = 0x0000FF00;
    static constexpr int EXC_SHIFT = 8;

    // Accrued exceptions (bits 7-0)
    static constexpr u32 AEXC_MASK = 0x000000FF;
}

// ---------------------------------------------------------------------------
// FPU exception bits (shared between FPCR enable, FPSR status, FPSR accrued)
// ---------------------------------------------------------------------------

namespace FPExc {
    static constexpr u8 BSNAN = 1 << 7;
    static constexpr u8 SNAN  = 1 << 6;
    static constexpr u8 OPERR = 1 << 5;
    static constexpr u8 OVFL  = 1 << 4;
    static constexpr u8 UNFL  = 1 << 3;
    static constexpr u8 DZ    = 1 << 2;
    static constexpr u8 INEX2 = 1 << 1;
    static constexpr u8 INEX1 = 1 << 0;
}

// ---------------------------------------------------------------------------
// FPU data format identifiers (extension word bits 12-10)
// ---------------------------------------------------------------------------

namespace FPFmt {
    static constexpr u8 Long     = 0;
    static constexpr u8 Single   = 1;
    static constexpr u8 Extended = 2;
    static constexpr u8 Packed   = 3;
    static constexpr u8 Word     = 4;
    static constexpr u8 Double   = 5;
    static constexpr u8 Byte     = 6;
}

// ---------------------------------------------------------------------------
// FP register helpers (operate on Registers::fp[n])
// ---------------------------------------------------------------------------

// Queries
inline bool fpIsZero(const FPReg &r)     { return (r.exp & 0x7FFF) == 0 && r.mantissa == 0; }
inline bool fpIsNeg(const FPReg &r)      { return r.exp & 0x8000; }
inline bool fpIsInf(const FPReg &r)      { return (r.exp & 0x7FFF) == FP_EXP_MAX && r.mantissa == 0; }
inline bool fpIsNaN(const FPReg &r)      { return (r.exp & 0x7FFF) == FP_EXP_MAX && r.mantissa != 0; }
inline bool fpIsSNaN(const FPReg &r)     { return fpIsNaN(r) && !(r.mantissa & (1ULL << 62)); }

// Set FPSR condition codes from an FP value
inline void fpSetCC(u32 &fpsr, const FPReg &r) {
    fpsr &= ~FPSR::CC_MASK;
    if (fpIsNaN(r))  fpsr |= FPSR::CC_NAN;
    else if (fpIsInf(r))  fpsr |= FPSR::CC_I | (fpIsNeg(r) ? FPSR::CC_N : 0);
    else if (fpIsZero(r)) fpsr |= FPSR::CC_Z | (fpIsNeg(r) ? FPSR::CC_N : 0);
    else if (fpIsNeg(r))  fpsr |= FPSR::CC_N;
}

// Special values
inline void fpSetZero(FPReg &r, bool neg = false)  { r.exp = neg ? 0x8000 : 0; r.mantissa = 0; }
inline void fpSetInf(FPReg &r, bool neg = false)    { r.exp = (neg ? 0x8000 : 0) | FP_EXP_MAX; r.mantissa = 0; }
inline void fpSetNaN(FPReg &r)                       { r.exp = FP_EXP_MAX; r.mantissa = 0xFFFFFFFFFFFFFFFFULL; }

// ---------------------------------------------------------------------------
// FMOVECR constant ROM (22 entries, from Motorola MC68881 User's Manual Table 5-1)
// ---------------------------------------------------------------------------

void fpMovecr(FPReg &dst, u8 offset);

// ---------------------------------------------------------------------------
// Format conversion
// ---------------------------------------------------------------------------

void fpFromSingle(FPReg &dst, u32 val);
void fpFromDouble(FPReg &dst, u64 val);
void fpFromLong(FPReg &dst, i32 val);
void fpFromWord(FPReg &dst, i16 val);
void fpFromByte(FPReg &dst, i8 val);

u32  fpToSingle(const FPReg &src);
u64  fpToDouble(const FPReg &src);
i32  fpToLong(const FPReg &src, u32 fpcr);
i16  fpToWord(const FPReg &src, u32 fpcr);
i8   fpToByte(const FPReg &src, u32 fpcr);

// ---------------------------------------------------------------------------
// Arithmetic (all operate on extended precision, set FPSR)
// ---------------------------------------------------------------------------

void fpAdd(FPReg &dst, const FPReg &src, u32 &fpsr);
void fpSub(FPReg &dst, const FPReg &src, u32 &fpsr);
void fpMul(FPReg &dst, const FPReg &src, u32 &fpsr);
void fpDiv(FPReg &dst, const FPReg &src, u32 &fpsr);
void fpSqrt(FPReg &dst, u32 &fpsr);
void fpAbs(FPReg &dst, u32 &fpsr);
void fpNeg(FPReg &dst, u32 &fpsr);
void fpCmp(const FPReg &dst, const FPReg &src, u32 &fpsr);
void fpTst(const FPReg &src, u32 &fpsr);
void fpInt(FPReg &dst, u32 fpcr, u32 &fpsr);
void fpIntrz(FPReg &dst, u32 &fpsr);

// Transcendental
void fpSin(FPReg &dst, u32 &fpsr);
void fpCos(FPReg &dst, u32 &fpsr);
void fpTan(FPReg &dst, u32 &fpsr);
void fpAsin(FPReg &dst, u32 &fpsr);
void fpAcos(FPReg &dst, u32 &fpsr);
void fpAtan(FPReg &dst, u32 &fpsr);
void fpSinh(FPReg &dst, u32 &fpsr);
void fpCosh(FPReg &dst, u32 &fpsr);
void fpTanh(FPReg &dst, u32 &fpsr);
void fpAtanh(FPReg &dst, u32 &fpsr);
void fpEtox(FPReg &dst, u32 &fpsr);
void fpEtoxm1(FPReg &dst, u32 &fpsr);
void fpTwotox(FPReg &dst, u32 &fpsr);
void fpTentox(FPReg &dst, u32 &fpsr);
void fpLogn(FPReg &dst, u32 &fpsr);
void fpLognp1(FPReg &dst, u32 &fpsr);
void fpLog2(FPReg &dst, u32 &fpsr);
void fpLog10(FPReg &dst, u32 &fpsr);

// Misc
void fpMod(FPReg &dst, const FPReg &src, u32 &fpsr);
void fpRem(FPReg &dst, const FPReg &src, u32 &fpsr);
void fpScale(FPReg &dst, const FPReg &src, u32 &fpsr);
void fpGetexp(FPReg &dst, u32 &fpsr);
void fpGetman(FPReg &dst, u32 &fpsr);
void fpSgldiv(FPReg &dst, const FPReg &src, u32 &fpsr);
void fpSglmul(FPReg &dst, const FPReg &src, u32 &fpsr);

// ---------------------------------------------------------------------------
// Packed BCD format conversion
// ---------------------------------------------------------------------------

void fpFromPacked(FPReg &dst, u32 w0, u32 w1, u32 w2);
void fpToPacked(const FPReg &src, i32 kFactor, u32 &w0, u32 &w1, u32 &w2);

// ---------------------------------------------------------------------------
// Rounding precision enforcement (68040 FSxxx/FDxxx)
// ---------------------------------------------------------------------------

void fpRoundToSingle(FPReg &dst);
void fpRoundToDouble(FPReg &dst);

// ---------------------------------------------------------------------------
// FPU exception check
// Returns exception vector number (48-54) if an enabled exception occurred,
// or 0 if no exception should be raised.
// ---------------------------------------------------------------------------

u8 fpCheckExceptions(u32 fpsr, u32 fpcr);

// ---------------------------------------------------------------------------
// FPU instruction cycle counts (from MC68881/68882 User's Manual, Table 9-3)
// These are the coprocessor execution times in clock cycles.
// The 68040 has a hardware FPU with much faster timing.
// ---------------------------------------------------------------------------

namespace FPUCycles {
    // 68881/68882 coprocessor clock cycles (approximate)
    static constexpr int FMOVE     = 4;
    static constexpr int FINT      = 4;
    static constexpr int FINTRZ    = 4;
    static constexpr int FABS      = 4;
    static constexpr int FNEG      = 4;
    static constexpr int FADD      = 28;
    static constexpr int FSUB      = 28;
    static constexpr int FMUL      = 71;
    static constexpr int FDIV      = 72;
    static constexpr int FSQRT     = 109;
    static constexpr int FMOD      = 72;
    static constexpr int FREM      = 72;
    static constexpr int FSCALE    = 4;
    static constexpr int FGETEXP   = 4;
    static constexpr int FGETMAN   = 4;
    static constexpr int FSGLDIV   = 54;
    static constexpr int FSGLMUL   = 53;
    static constexpr int FCMP      = 28;
    static constexpr int FTST      = 4;
    static constexpr int FSIN      = 392;
    static constexpr int FCOS      = 392;
    static constexpr int FTAN      = 441;
    static constexpr int FSINCOS   = 497;
    static constexpr int FASIN     = 168;
    static constexpr int FACOS     = 168;
    static constexpr int FATAN     = 168;
    static constexpr int FSINH     = 168;
    static constexpr int FCOSH     = 168;
    static constexpr int FTANH     = 168;
    static constexpr int FATANH    = 168;
    static constexpr int FETOX     = 168;
    static constexpr int FETOXM1   = 168;
    static constexpr int FTWOTOX   = 168;
    static constexpr int FTENTOX   = 168;
    static constexpr int FLOGN     = 168;
    static constexpr int FLOGNP1   = 168;
    static constexpr int FLOG2     = 168;
    static constexpr int FLOG10    = 168;
    static constexpr int FMOVECR   = 4;
    static constexpr int FMOVEM    = 4;  // per register: +4
    static constexpr int FBCC      = 4;
    static constexpr int FDBCC     = 4;
    static constexpr int FSCC      = 4;
    static constexpr int FSAVE     = 4;
    static constexpr int FRESTORE  = 4;

    // 68040 hardware FPU (much faster)
    static constexpr int FADD_040  = 3;
    static constexpr int FSUB_040  = 3;
    static constexpr int FMUL_040  = 3;
    static constexpr int FDIV_040  = 38;
    static constexpr int FSQRT_040 = 38;
}

// Return cycle count for an FPU opcode (cmd field, bits 6-0)
int fpCycleCount(u8 cmd, bool is040);

// ---------------------------------------------------------------------------
// SoftFloat state management (call before operations to sync FPCR)
// ---------------------------------------------------------------------------

void fpSyncState(u32 fpcr);

} // namespace moira
