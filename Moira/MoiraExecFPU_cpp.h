// -----------------------------------------------------------------------------
// This file is part of Moira - A Motorola 68k emulator
//
// Copyright (C) Dirk W. Hoffmann. www.dirkwhoffmann.de
// Published under the terms of the MIT License
// -----------------------------------------------------------------------------
//
// FPU execution — clean-room implementation
// Based on M68000 Family Programmer's Reference Manual (Motorola, 1992)
// -----------------------------------------------------------------------------

#include "MoiraFPU.h"

bool
Moira::isValidExtFPU(Instr I, Mode M, u16 op, u32 ext) const
{
    auto cod  = xxx_____________ (ext);
    auto mode = ___xx___________ (ext);
    auto fmt  = ___xxx__________ (ext);
    auto lst  = ___xxx__________ (ext);
    auto cmd  = _________xxxxxxx (ext);

    switch (I) {

        case Instr::FDBcc:
        case Instr::FScc:
        case Instr::FTRAPcc:

            return (ext & 0xFFE0) == 0;

        case Instr::FMOVECR:

            return (op & 0x3F) == 0;

        case Instr::FMOVE:

            switch (cod) {

                case 0b010:

                    if (M == Mode::IP) break;
                    return true;

                case 0b000:

                    if (cmd == 0 && cod == 0 && (op & 0x3F)) break;
                    return true;

                case 0b011:

                    if (fmt != 0b011 && fmt != 0b111 && (ext & 0x7F)) break;

                    if (M == Mode::DN) {
                        if (fmt == 0b010 || fmt == 0b011 || fmt == 0b101 || fmt == 0b111) break;
                    }
                    if (M == Mode::AN) {
                        if (fmt == 0b011 || fmt == 0b111) break;
                    }
                    if (M == Mode::DIPC || M == Mode::IXPC || M == Mode::IM || M == Mode::IP) {
                        break;
                    } else {
                        if (fmt == 0b111 && (ext & 0xF)) break;
                    }

                    return true;
            }

        case Instr::FMOVEM:

            switch (cod) {

                case 0b101:
                {

                    if (ext & 0x3FF) break;

                    if (M == Mode::DN || M == Mode::AN) {
                        if (lst != 0b000 && lst != 0b001 && lst != 0b010 && lst != 0b100) break;
                    }
                    if (M == Mode::DIPC || M == Mode::IXPC || M == Mode::IM || M == Mode::IP) {
                        break;
                    }
                    return true;
                }
                case 0b100:

                    if (ext & 0x3FF) break;
                    if (M == Mode::IP) break;
                    return true;

                case 0b110:
                case 0b111:

                    if (ext & 0x0700) break;
                    if (mode == 3 && (ext & 0x8F)) break;

                    if (M == Mode::DN || M == Mode::AN) {
                        break;
                    }
                    if (M == Mode::DIPC || M == Mode::IXPC || M == Mode::IM || M == Mode::IP) {
                        break;
                    }
                    if (M == Mode::AI) {
                        if (mode == 0 || mode == 1) break;
                    }
                    if (M == Mode::PI) {
                        if (mode == 0 || mode == 1 || cod == 0b111) break;
                    }
                    if (M == Mode::PD) {
                        if (cod == 0b110) break;
                        if (cod == 0b111 && (mode == 1) && (ext & 0x8F)) break;
                        if (cod == 0b111 && (mode == 2 || mode == 3)) break;
                    }
                    if (M == Mode::DI || M == Mode::IX || M == Mode::AW || M == Mode::AL) {
                        if (mode == 0 || mode == 1) break;
                    }
                    return true;
            }
            return false;

        default:
            fatalError;
    }
}

// ---------------------------------------------------------------------------
// FPU helper: read source operand into extended precision
// ---------------------------------------------------------------------------

template <Core C, Mode M, Size S> void
Moira::fpReadSource(u16 opcode, u32 ext, FPReg &src)
{
    auto cod = xxx_____________ (ext);
    auto fmt = ___xxx__________ (ext);
    auto srcReg = ____xxx_________ (ext);

    if (cod == 0b000) {
        // Register to register: source is FP register
        src = reg.fp[srcReg & 7];
        return;
    }

    // Memory to register: read EA and convert based on format
    u32 ea;
    u32 data32;

    switch (fmt) {

        case FPFmt::Byte: {
            readOp<C, M, Byte>(_____________xxx(opcode), &ea, &data32);
            fpFromByte(src, (i8)data32);
            break;
        }
        case FPFmt::Word: {
            readOp<C, M, Word>(_____________xxx(opcode), &ea, &data32);
            fpFromWord(src, (i16)data32);
            break;
        }
        case FPFmt::Long: {
            readOp<C, M, Long>(_____________xxx(opcode), &ea, &data32);
            fpFromLong(src, (i32)data32);
            break;
        }
        case FPFmt::Single: {
            readOp<C, M, Long>(_____________xxx(opcode), &ea, &data32);
            fpFromSingle(src, data32);
            break;
        }
        case FPFmt::Double: {
            // Read 64 bits (two longs, big-endian)
            readOp<C, M, Long>(_____________xxx(opcode), &ea, &data32);
            u64 hi = (u64)data32 << 32;
            u32 lo32;
            readOp<C, M, Long>(_____________xxx(opcode), &ea, &lo32);
            fpFromDouble(src, hi | lo32);
            break;
        }
        case FPFmt::Extended: {
            // Read 96 bits (three longs: exp+zero, mantissa high, mantissa low)
            u32 w0, w1, w2;
            readOp<C, M, Long>(_____________xxx(opcode), &ea, &w0);
            readOp<C, M, Long>(_____________xxx(opcode), &ea, &w1);
            readOp<C, M, Long>(_____________xxx(opcode), &ea, &w2);
            src.exp = (u16)(w0 >> 16);
            src.mantissa = ((u64)w1 << 32) | w2;
            break;
        }
        default:
            fpSetNaN(src);
            break;
    }
}

// ---------------------------------------------------------------------------
// execFGen: Main FPU arithmetic dispatch
// Extension word format: [cod:3][src:3][dst:3][opcode:7]
// cod=000: reg-to-reg, cod=010: mem-to-reg (+ arithmetic), cod=011: reg-to-mem
// ---------------------------------------------------------------------------

template <Core C, Instr I, Mode M, Size S> void
Moira::execFGen(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }

    u32 ext = (u32)readI<C, Word>();
    auto cod = xxx_____________ (ext);
    auto dstReg = ______xxx_______ (ext) & 7;
    auto cmd = _________xxxxxxx (ext);

    reg.fpiar = reg.pc0;

    // Clear exception status before each operation
    reg.fpsr &= ~FPSR::EXC_MASK;

    if (cod == 0b010 || cod == 0b000) {

        // Memory-to-register or register-to-register operation
        FPReg src;
        fpReadSource<C, M, S>(opcode, ext, src);

        // Decode the arithmetic operation from bits 6-0
        switch (cmd) {

            case 0x00: reg.fp[dstReg] = src; fpSetCC(reg.fpsr, reg.fp[dstReg]); break; // FMOVE
            case 0x01: fpInt(reg.fp[dstReg] = src, reg.fpcr, reg.fpsr); break;          // FINT
            case 0x02: fpSinh(reg.fp[dstReg] = src, reg.fpsr); break;                   // FSINH
            case 0x03: fpIntrz(reg.fp[dstReg] = src, reg.fpsr); break;                  // FINTRZ
            case 0x04: fpSqrt(reg.fp[dstReg] = src, reg.fpsr); break;                   // FSQRT
            case 0x06: fpLognp1(reg.fp[dstReg] = src, reg.fpsr); break;                 // FLOGNP1
            case 0x08: fpEtoxm1(reg.fp[dstReg] = src, reg.fpsr); break;                 // FETOXM1
            case 0x09: fpTanh(reg.fp[dstReg] = src, reg.fpsr); break;                   // FTANH
            case 0x0A: fpAtan(reg.fp[dstReg] = src, reg.fpsr); break;                   // FATAN
            case 0x0C: fpAcos(reg.fp[dstReg] = src, reg.fpsr); break;                   // FACOS
            case 0x0D: fpAtanh(reg.fp[dstReg] = src, reg.fpsr); break;                  // FATANH
            case 0x0E: fpSin(reg.fp[dstReg] = src, reg.fpsr); break;                    // FSIN
            case 0x0F: fpTan(reg.fp[dstReg] = src, reg.fpsr); break;                    // FTAN
            case 0x10: fpEtox(reg.fp[dstReg] = src, reg.fpsr); break;                   // FETOX
            case 0x11: fpTwotox(reg.fp[dstReg] = src, reg.fpsr); break;                 // FTWOTOX
            case 0x12: fpTentox(reg.fp[dstReg] = src, reg.fpsr); break;                 // FTENTOX
            case 0x14: fpLogn(reg.fp[dstReg] = src, reg.fpsr); break;                   // FLOGN
            case 0x15: fpLog10(reg.fp[dstReg] = src, reg.fpsr); break;                  // FLOG10
            case 0x16: fpLog2(reg.fp[dstReg] = src, reg.fpsr); break;                   // FLOG2
            case 0x18: fpAbs(reg.fp[dstReg] = src, reg.fpsr); break;                    // FABS
            case 0x19: fpCosh(reg.fp[dstReg] = src, reg.fpsr); break;                   // FCOSH
            case 0x1A: fpNeg(reg.fp[dstReg] = src, reg.fpsr); break;                    // FNEG
            case 0x1C: fpAcos(reg.fp[dstReg] = src, reg.fpsr); break;                   // FACOS
            case 0x1D: fpCos(reg.fp[dstReg] = src, reg.fpsr); break;                    // FCOS
            case 0x1E: fpGetexp(reg.fp[dstReg] = src, reg.fpsr); break;                 // FGETEXP
            case 0x1F: fpGetman(reg.fp[dstReg] = src, reg.fpsr); break;                 // FGETMAN
            case 0x20: fpDiv(reg.fp[dstReg], src, reg.fpsr); break;                     // FDIV
            case 0x21: fpMod(reg.fp[dstReg], src, reg.fpsr); break;                     // FMOD
            case 0x22: fpAdd(reg.fp[dstReg], src, reg.fpsr); break;                     // FADD
            case 0x23: fpMul(reg.fp[dstReg], src, reg.fpsr); break;                     // FMUL
            case 0x24: fpSgldiv(reg.fp[dstReg], src, reg.fpsr); break;                  // FSGLDIV
            case 0x25: fpRem(reg.fp[dstReg], src, reg.fpsr); break;                     // FREM
            case 0x26: fpScale(reg.fp[dstReg], src, reg.fpsr); break;                   // FSCALE
            case 0x27: fpSglmul(reg.fp[dstReg], src, reg.fpsr); break;                  // FSGLMUL
            case 0x28: case 0x29: case 0x2A: case 0x2B:
            case 0x2C: case 0x2D: case 0x2E: case 0x2F:
                fpSub(reg.fp[dstReg], src, reg.fpsr); break;                            // FSUB
            case 0x38: case 0x39: case 0x3C: case 0x3D:
                fpCmp(reg.fp[dstReg], src, reg.fpsr); break;                            // FCMP
            case 0x3A: case 0x3B: case 0x3E: case 0x3F:
                fpTst(src, reg.fpsr); break;                                             // FTST

            // 68040 single/double precision variants
            case 0x40: reg.fp[dstReg] = src; fpSetCC(reg.fpsr, reg.fp[dstReg]); break; // FSMOVE
            case 0x44: reg.fp[dstReg] = src; fpSetCC(reg.fpsr, reg.fp[dstReg]); break; // FDMOVE
            case 0x41: fpSqrt(reg.fp[dstReg] = src, reg.fpsr); break;                   // FSSQRT
            case 0x45: fpSqrt(reg.fp[dstReg] = src, reg.fpsr); break;                   // FDSQRT
            case 0x58: fpAbs(reg.fp[dstReg] = src, reg.fpsr); break;                    // FSABS
            case 0x5A: fpNeg(reg.fp[dstReg] = src, reg.fpsr); break;                    // FSNEG
            case 0x5C: fpAbs(reg.fp[dstReg] = src, reg.fpsr); break;                    // FDABS
            case 0x5E: fpNeg(reg.fp[dstReg] = src, reg.fpsr); break;                    // FDNEG
            case 0x60: fpDiv(reg.fp[dstReg], src, reg.fpsr); break;                     // FSDIV
            case 0x62: fpAdd(reg.fp[dstReg], src, reg.fpsr); break;                     // FSADD
            case 0x63: fpMul(reg.fp[dstReg], src, reg.fpsr); break;                     // FSMUL
            case 0x64: fpDiv(reg.fp[dstReg], src, reg.fpsr); break;                     // FDDIV
            case 0x66: fpAdd(reg.fp[dstReg], src, reg.fpsr); break;                     // FDADD
            case 0x67: fpMul(reg.fp[dstReg], src, reg.fpsr); break;                     // FDMUL
            case 0x68: fpSub(reg.fp[dstReg], src, reg.fpsr); break;                     // FSSUB
            case 0x6C: fpSub(reg.fp[dstReg], src, reg.fpsr); break;                     // FDSUB

            // FSINCOS: sin → FPc, cos → FPn
            case 0x30: case 0x31: case 0x32: case 0x33:
            case 0x34: case 0x35: case 0x36: case 0x37: {
                int cosReg = cmd & 7;
                reg.fp[dstReg] = src;
                fpSin(reg.fp[dstReg], reg.fpsr);
                reg.fp[cosReg] = src;
                fpCos(reg.fp[cosReg], reg.fpsr);
                break;
            }

            default:
                break;
        }
    }
    else if (cod == 0b011) {
        // Register to memory (FMOVE FPn,<ea>) — handled by execFMove
    }
    else if (cod == 0b110 || cod == 0b111 || cod == 0b100 || cod == 0b101) {
        // FMOVEM — handled by execFMovem
    }

    prefetch<C, POLL>();

    CYCLES_68020(4)

    FINALIZE
}

// ---------------------------------------------------------------------------
// FMOVECR: Load FPU constant ROM
// ---------------------------------------------------------------------------

template <Core C, Instr I, Mode M, Size S> void
Moira::execFMovecr(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }

    u32 ext = (u32)readI<C, Word>();
    auto dstReg = ______xxx_______ (ext) & 7;
    auto offset = _________xxxxxxx (ext);

    reg.fpiar = reg.pc0;
    reg.fpsr &= ~FPSR::EXC_MASK;

    fpMovecr(reg.fp[dstReg], (u8)offset);
    fpSetCC(reg.fpsr, reg.fp[dstReg]);

    prefetch<C, POLL>();
    CYCLES_68020(4)
    FINALIZE
}

// ---------------------------------------------------------------------------
// FBcc: Branch on FPU condition
// ---------------------------------------------------------------------------

template <Core C, Instr I, Mode M, Size S> void
Moira::execFBcc(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }

    u8 cond = opcode & 0x3F;
    i32 disp;

    if constexpr (S == Word) {
        disp = (i16)readI<C, Word>();
    } else {
        disp = (i32)readI<C, Long>();
    }

    reg.fpiar = reg.pc0;

    // Evaluate FPU condition from FPSR condition codes
    bool taken = false;
    u32 cc = reg.fpsr & FPSR::CC_MASK;
    bool N = cc & FPSR::CC_N;
    bool Z = cc & FPSR::CC_Z;
    bool Inf = cc & FPSR::CC_I;
    bool NaN = cc & FPSR::CC_NAN;

    switch (cond & 0x1F) {
        case 0x00: taken = false; break;                          // F
        case 0x01: taken = Z; break;                              // EQ
        case 0x02: taken = !(NaN || Z || N); break;               // OGT
        case 0x03: taken = Z || !(NaN || N); break;               // OGE
        case 0x04: taken = N && !(NaN || Z); break;               // OLT
        case 0x05: taken = Z || (N && !NaN); break;               // OLE
        case 0x06: taken = !(NaN || Z); break;                    // OGL
        case 0x07: taken = !NaN; break;                           // OR
        case 0x08: taken = NaN; break;                            // UN
        case 0x09: taken = NaN || Z; break;                       // UEQ
        case 0x0A: taken = NaN || !(N || Z); break;               // UGT
        case 0x0B: taken = NaN || Z || !N; break;                 // UGE
        case 0x0C: taken = NaN || (N && !Z); break;               // ULT
        case 0x0D: taken = NaN || Z || N; break;                  // ULE
        case 0x0E: taken = !Z; break;                             // NE (SNE)
        case 0x0F: taken = true; break;                           // T (ST)
        case 0x10: taken = false; break;                          // SF
        case 0x11: taken = Z; break;                              // SEQ
        case 0x12: taken = !(NaN || Z || N); break;               // GT
        case 0x13: taken = Z || !(NaN || N); break;               // GE
        case 0x14: taken = N && !(NaN || Z); break;               // LT
        case 0x15: taken = Z || (N && !NaN); break;               // LE
        case 0x16: taken = !(NaN || Z); break;                    // GL
        case 0x17: taken = !NaN; break;                           // GLE
        case 0x18: taken = NaN; break;                            // NGLE
        case 0x19: taken = NaN || Z; break;                       // NGL
        case 0x1A: taken = NaN || !(N || Z); break;               // NLE
        case 0x1B: taken = NaN || Z || !N; break;                 // NLT
        case 0x1C: taken = NaN || (N && !Z); break;               // NGE
        case 0x1D: taken = NaN || Z || N; break;                  // NGT
        case 0x1E: taken = !Z; break;                             // NE
        case 0x1F: taken = true; break;                           // T
    }

    if (taken) {
        reg.pc = reg.pc0 + 2 + disp;
    }

    prefetch<C, POLL>();
    CYCLES_68020(4)
    FINALIZE
}

// ---------------------------------------------------------------------------
// FNop
// ---------------------------------------------------------------------------

template <Core C, Instr I, Mode M, Size S> void
Moira::execFNop(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }

    (void)readI<C, Word>(); // consume extension word

    prefetch<C, POLL>();
    CYCLES_68020(4)
    FINALIZE
}

// ---------------------------------------------------------------------------
// Stubs for instructions not yet fully implemented
// ---------------------------------------------------------------------------

template <Core C, Instr I, Mode M, Size S> void
Moira::execFDbcc(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }
    // TODO: Implement FDBcc
    execLineF<C, I, M, S>(opcode);
    FINALIZE
}

template <Core C, Instr I, Mode M, Size S> void
Moira::execFRestore(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }
    // TODO: Implement FRESTORE
    (void)readI<C, Word>();
    prefetch<C, POLL>();
    CYCLES_68020(4)
    FINALIZE
}

template <Core C, Instr I, Mode M, Size S> void
Moira::execFSave(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }
    SUPERVISOR_MODE_ONLY
    // TODO: Implement FSAVE
    (void)readI<C, Word>();
    prefetch<C, POLL>();
    CYCLES_68020(4)
    FINALIZE
}

template <Core C, Instr I, Mode M, Size S> void
Moira::execFScc(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }
    // TODO: Implement FScc
    execLineF<C, I, M, S>(opcode);
    FINALIZE
}

template <Core C, Instr I, Mode M, Size S> void
Moira::execFTrapcc(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }
    // TODO: Implement FTRAPcc
    execLineF<C, I, M, S>(opcode);
    FINALIZE
}

template <Core C, Instr I, Mode M, Size S> void
Moira::execFMove(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }
    // TODO: Implement FMOVE reg-to-mem
    execLineF<C, I, M, S>(opcode);
    FINALIZE
}

template <Core C, Instr I, Mode M, Size S> void
Moira::execFMovem(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }
    // TODO: Implement FMOVEM
    execLineF<C, I, M, S>(opcode);
    FINALIZE
}

template <Core C, Instr I, Mode M, Size S> void
Moira::execFGeneric(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }
    execFGen<C, I, M, S>(opcode);
    FINALIZE
}
