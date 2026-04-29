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
        case FPFmt::Packed: {
            // Read 96 bits packed BCD (3 longwords)
            u32 w0, w1, w2;
            readOp<C, M, Long>(_____________xxx(opcode), &ea, &w0);
            readOp<C, M, Long>(_____________xxx(opcode), &ea, &w1);
            readOp<C, M, Long>(_____________xxx(opcode), &ea, &w2);
            fpFromPacked(src, w0, w1, w2);
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

    // Sync SoftFloat state with current FPCR settings
    fpSyncState(reg.fpcr);

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
            case 0x40: reg.fp[dstReg] = src; fpRoundToSingle(reg.fp[dstReg]); fpSetCC(reg.fpsr, reg.fp[dstReg]); break; // FSMOVE
            case 0x44: reg.fp[dstReg] = src; fpRoundToDouble(reg.fp[dstReg]); fpSetCC(reg.fpsr, reg.fp[dstReg]); break; // FDMOVE
            case 0x41: fpSqrt(reg.fp[dstReg] = src, reg.fpsr); fpRoundToSingle(reg.fp[dstReg]); break; // FSSQRT
            case 0x45: fpSqrt(reg.fp[dstReg] = src, reg.fpsr); fpRoundToDouble(reg.fp[dstReg]); break; // FDSQRT
            case 0x58: fpAbs(reg.fp[dstReg] = src, reg.fpsr); fpRoundToSingle(reg.fp[dstReg]); break;  // FSABS
            case 0x5A: fpNeg(reg.fp[dstReg] = src, reg.fpsr); fpRoundToSingle(reg.fp[dstReg]); break;  // FSNEG
            case 0x5C: fpAbs(reg.fp[dstReg] = src, reg.fpsr); fpRoundToDouble(reg.fp[dstReg]); break;  // FDABS
            case 0x5E: fpNeg(reg.fp[dstReg] = src, reg.fpsr); fpRoundToDouble(reg.fp[dstReg]); break;  // FDNEG
            case 0x60: fpDiv(reg.fp[dstReg], src, reg.fpsr); fpRoundToSingle(reg.fp[dstReg]); break;   // FSDIV
            case 0x62: fpAdd(reg.fp[dstReg], src, reg.fpsr); fpRoundToSingle(reg.fp[dstReg]); break;   // FSADD
            case 0x63: fpMul(reg.fp[dstReg], src, reg.fpsr); fpRoundToSingle(reg.fp[dstReg]); break;   // FSMUL
            case 0x64: fpDiv(reg.fp[dstReg], src, reg.fpsr); fpRoundToDouble(reg.fp[dstReg]); break;   // FDDIV
            case 0x66: fpAdd(reg.fp[dstReg], src, reg.fpsr); fpRoundToDouble(reg.fp[dstReg]); break;   // FDADD
            case 0x67: fpMul(reg.fp[dstReg], src, reg.fpsr); fpRoundToDouble(reg.fp[dstReg]); break;   // FDMUL
            case 0x68: fpSub(reg.fp[dstReg], src, reg.fpsr); fpRoundToSingle(reg.fp[dstReg]); break;   // FSSUB
            case 0x6C: fpSub(reg.fp[dstReg], src, reg.fpsr); fpRoundToDouble(reg.fp[dstReg]); break;   // FDSUB

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

    // Check for enabled FPU exceptions
    if (u8 vec = fpCheckExceptions(reg.fpsr, reg.fpcr)) {
        execException<C>((M68kException)vec);
        FINALIZE
    }

    prefetch<C, POLL>();

    // Cycle-accurate FPU timing based on instruction opcode
    bool is040 = (cpuModel == Model::M68040 || cpuModel == Model::M68EC040 || cpuModel == Model::M68LC040);
    CYCLES_68020(fpCycleCount((u8)cmd, is040))

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

// Forward declaration
static bool evalFPCond(u32 fpsr, u8 cond);

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

    if (evalFPCond(reg.fpsr, cond)) {
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

// ---------------------------------------------------------------------------
// FPU condition evaluator (shared by FBcc, FDBcc, FScc, FTRAPcc)
// ---------------------------------------------------------------------------

static bool
evalFPCond(u32 fpsr, u8 cond)
{
    u32 cc = fpsr & FPSR::CC_MASK;
    bool N = cc & FPSR::CC_N;
    bool Z = cc & FPSR::CC_Z;
    bool NaN = cc & FPSR::CC_NAN;

    switch (cond & 0x1F) {
        case 0x00: return false;                          // F
        case 0x01: return Z;                              // EQ
        case 0x02: return !(NaN || Z || N);               // OGT
        case 0x03: return Z || !(NaN || N);               // OGE
        case 0x04: return N && !(NaN || Z);               // OLT
        case 0x05: return Z || (N && !NaN);               // OLE
        case 0x06: return !(NaN || Z);                    // OGL
        case 0x07: return !NaN;                           // OR
        case 0x08: return NaN;                            // UN
        case 0x09: return NaN || Z;                       // UEQ
        case 0x0A: return NaN || !(N || Z);               // UGT
        case 0x0B: return NaN || Z || !N;                 // UGE
        case 0x0C: return NaN || (N && !Z);               // ULT
        case 0x0D: return NaN || Z || N;                  // ULE
        case 0x0E: return !Z;                             // NE
        case 0x0F: return true;                           // T
        default:   return false;
    }
}

template <Core C, Instr I, Mode M, Size S> void
Moira::execFDbcc(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }

    u32 ext = (u32)readI<C, Word>();
    u8 cond = (u8)(ext & 0x3F);
    i16 disp = (i16)readI<C, Word>();
    int dn = _____________xxx(opcode);

    reg.fpiar = reg.pc0;

    if (!evalFPCond(reg.fpsr, cond)) {
        i16 counter = (i16)readD<Word>(dn);
        counter--;
        writeD<Word>(dn, (u32)(u16)counter);
        if (counter != -1) {
            reg.pc = reg.pc0 + 4 + disp;  // +4 = past opcode + ext word
        }
    }

    prefetch<C, POLL>();
    CYCLES_68020(4)
    FINALIZE
}

template <Core C, Instr I, Mode M, Size S> void
Moira::execFScc(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }

    u32 ext = (u32)readI<C, Word>();
    u8 cond = (u8)(ext & 0x3F);
    int dst = _____________xxx(opcode);

    reg.fpiar = reg.pc0;

    u8 result = evalFPCond(reg.fpsr, cond) ? 0xFF : 0x00;
    writeOp<C, M, Byte>(dst, result);

    prefetch<C, POLL>();
    CYCLES_68020(4)
    FINALIZE
}

template <Core C, Instr I, Mode M, Size S> void
Moira::execFTrapcc(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }

    u32 ext = (u32)readI<C, Word>();
    u8 cond = (u8)(ext & 0x3F);

    // Consume operand based on size
    if constexpr (S == Word) { (void)readI<C, Word>(); }
    else if constexpr (S == Long) { (void)readI<C, Long>(); }

    reg.fpiar = reg.pc0;

    if (evalFPCond(reg.fpsr, cond)) {
        execException<C>(M68kException::TRAPV);
    }

    prefetch<C, POLL>();
    CYCLES_68020(4)
    FINALIZE
}

template <Core C, Instr I, Mode M, Size S> void
Moira::execFRestore(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }

    // FRESTORE: restore FPU state from memory
    // Frame format (from Motorola M68000 Family Programmer's Reference Manual):
    //   First long: [version:8][size:8][reserved:16]
    //   If size == 0: null frame (FPU reset to idle)
    //   If size > 0: idle/busy frame (internal state follows)

    int src = _____________xxx(opcode);
    u32 ea = computeEA<C, M, Long>(src);

    u32 header = readM<C, M, Long>(ea);
    u8 frameSize = (u8)((header >> 16) & 0xFF);

    if (frameSize == 0x00) {
        // Null frame: reset FPU to idle state
        for (int i = 0; i < 8; i++) { reg.fp[i].exp = 0; reg.fp[i].mantissa = 0; }
        reg.fpcr = 0;
        reg.fpsr = 0;
        reg.fpiar = 0;
        if constexpr (M == Mode::PI) { writeA(src, ea + 4); }
    } else {
        // Idle or busy frame: skip the internal state bytes
        // The FP data registers are NOT part of the frame — they're saved
        // separately via FMOVEM. FSAVE only saves internal pipeline state.
        // For our implementation, we just consume the frame and keep current state.
        u32 totalSize = 4 + (u32)frameSize;
        if constexpr (M == Mode::PI) { writeA(src, ea + totalSize); }
    }

    prefetch<C, POLL>();
    CYCLES_68020(4)
    FINALIZE
}

template <Core C, Instr I, Mode M, Size S> void
Moira::execFSave(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }
    SUPERVISOR_MODE_ONLY

    // FSAVE: save FPU internal state to memory
    // We generate a minimal idle frame since our FPU has no pending exceptions.
    //
    // 68040 idle frame: 4 bytes (just the header)
    //   [version:8][0x00:8][0x0000:16]
    //
    // 68881/68882 idle frame: 28 bytes (header + 24 bytes internal state)
    //   [version:8][0x18:8][0x0000:16] + 24 bytes of zeros

    int dst = _____________xxx(opcode);

    // Determine frame format based on CPU model
    bool is040 = (cpuModel == Model::M68040 || cpuModel == Model::M68EC040 || cpuModel == Model::M68LC040);

    if (is040) {
        // 68040: 4-byte idle frame
        u32 header = 0x41000000;  // version=$41, size=0 (idle)
        u32 ea = computeEA<C, M, Long>(dst);
        if constexpr (M == Mode::PD) {
            ea -= 4;
            writeM<C, M, Long>(ea, header);
            writeA(dst, ea);
        } else {
            writeM<C, M, Long>(ea, header);
        }
    } else {
        // 68881/68882: 28-byte idle frame
        u32 frameSize = 0x18;  // 24 bytes of internal state
        u32 header = (0x1F << 24) | (frameSize << 16);  // version=$1F

        if constexpr (M == Mode::PD) {
            u32 ea = readA(dst);
            ea -= 4 + frameSize;
            writeM<C, M, Long>(ea, header);
            // Write 24 bytes of zeros (internal state — we have none)
            for (u32 i = 4; i < 4 + frameSize; i += 4) {
                writeM<C, M, Long>(ea + i, 0);
            }
            writeA(dst, ea);
        } else {
            u32 ea = computeEA<C, M, Long>(dst);
            writeM<C, M, Long>(ea, header);
            for (u32 i = 4; i < 4 + frameSize; i += 4) {
                writeM<C, M, Long>(ea + i, 0);
            }
        }
    }

    prefetch<C, POLL>();
    CYCLES_68020(4)
    FINALIZE
}

template <Core C, Instr I, Mode M, Size S> void
Moira::execFMove(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }

    u32 ext = (u32)readI<C, Word>();
    auto cod = xxx_____________ (ext);
    auto fmt = ___xxx__________ (ext);
    auto srcReg = ______xxx_______ (ext) & 7;

    reg.fpiar = reg.pc0;
    reg.fpsr &= ~FPSR::EXC_MASK;

    if (cod == 0b011) {
        // FP register → memory
        auto &src = reg.fp[srcReg];
        int dst = _____________xxx(opcode);
        u32 ea;

        switch (fmt) {
            case FPFmt::Byte: {
                i8 val = fpToByte(src, reg.fpcr);
                ea = computeEA<C, M, Byte>(dst);
                writeM<C, M, Byte>(ea, (u32)(u8)val);
                break;
            }
            case FPFmt::Word: {
                i16 val = fpToWord(src, reg.fpcr);
                ea = computeEA<C, M, Word>(dst);
                writeM<C, M, Word>(ea, (u32)(u16)val);
                break;
            }
            case FPFmt::Long: {
                i32 val = fpToLong(src, reg.fpcr);
                ea = computeEA<C, M, Long>(dst);
                writeM<C, M, Long>(ea, (u32)val);
                break;
            }
            case FPFmt::Single: {
                u32 val = fpToSingle(src);
                ea = computeEA<C, M, Long>(dst);
                writeM<C, M, Long>(ea, val);
                break;
            }
            case FPFmt::Double: {
                u64 val = fpToDouble(src);
                ea = computeEA<C, M, Long>(dst);
                writeM<C, M, Long>(ea, (u32)(val >> 32));
                writeM<C, M, Long>(ea + 4, (u32)val);
                break;
            }
            case FPFmt::Extended: {
                u32 w0 = (u32)src.exp << 16;
                u32 w1 = (u32)(src.mantissa >> 32);
                u32 w2 = (u32)src.mantissa;
                ea = computeEA<C, M, Long>(dst);
                writeM<C, M, Long>(ea, w0);
                writeM<C, M, Long>(ea + 4, w1);
                writeM<C, M, Long>(ea + 8, w2);
                break;
            }
            case FPFmt::Packed: {
                // k-factor from extension word bits 6-0 (static) or Dn (dynamic)
                i32 kFactor;
                if (ext & 0x1000) {
                    // Dynamic: k-factor from data register
                    int kReg = (ext >> 4) & 7;
                    kFactor = (i32)(i8)(readD<Long>(kReg) & 0x7F);
                } else {
                    // Static: k-factor from extension word (7-bit signed)
                    kFactor = (i32)(i8)((ext & 0x7F) | ((ext & 0x40) ? 0x80 : 0));
                }
                u32 w0, w1, w2;
                fpToPacked(src, kFactor, w0, w1, w2);
                ea = computeEA<C, M, Long>(dst);
                writeM<C, M, Long>(ea, w0);
                writeM<C, M, Long>(ea + 4, w1);
                writeM<C, M, Long>(ea + 8, w2);
                break;
            }
            default:
                break;
        }
    }
    else if (cod == 0b100 || cod == 0b101) {
        // FMOVE to/from FPCR/FPSR/FPIAR (system registers)
        int dst = _____________xxx(opcode);
        u32 regList = ___xxx__________ (ext);

        if (cod == 0b101) {
            // Memory/Dn → system register
            u32 ea, data;
            readOp<C, M, Long>(dst, &ea, &data);
            if (regList & 0b100) reg.fpcr  = data;
            if (regList & 0b010) reg.fpsr  = data;
            if (regList & 0b001) reg.fpiar = data;
        } else {
            // System register → memory/Dn
            u32 data = 0;
            if (regList & 0b100) data = reg.fpcr;
            else if (regList & 0b010) data = reg.fpsr;
            else if (regList & 0b001) data = reg.fpiar;
            writeOp<C, M, Long>(dst, data);
        }
    }

    prefetch<C, POLL>();
    CYCLES_68020(4)
    FINALIZE
}

template <Core C, Instr I, Mode M, Size S> void
Moira::execFMovem(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }

    u32 ext = (u32)readI<C, Word>();
    auto cod = xxx_____________ (ext);
    auto mode = ___xx___________ (ext);
    int src = _____________xxx(opcode);

    reg.fpiar = reg.pc0;

    if (cod == 0b110 || cod == 0b111) {
        // FMOVEM FP registers ↔ memory
        // Mode bits [12:11]: 0/2 = static list from ext word, 1/3 = dynamic from Dn
        u8 regMask;
        if (mode & 1) {
            // Dynamic: register list from data register Dn
            int dn = (ext >> 4) & 7;
            regMask = (u8)(readD<Long>(dn) & 0xFF);
        } else {
            // Static: register list from extension word
            regMask = (u8)(ext & 0xFF);
        }
        u32 ea = computeEA<C, M, Long>(src);

        if (cod == 0b110) {
            // Memory → FP registers (load)
            for (int i = 7; i >= 0; i--) {
                if (!(regMask & (1 << (7 - i)))) continue;
                u32 w0 = readM<C, M, Long>(ea);
                u32 w1 = readM<C, M, Long>(ea + 4);
                u32 w2 = readM<C, M, Long>(ea + 8);
                reg.fp[i].exp = (u16)(w0 >> 16);
                reg.fp[i].mantissa = ((u64)w1 << 32) | w2;
                ea += 12;
            }
        } else {
            // FP registers → memory (store)
            if constexpr (M == Mode::PD) {
                // Pre-decrement: store in reverse order
                for (int i = 0; i <= 7; i++) {
                    if (!(regMask & (1 << (7 - i)))) continue;
                    ea -= 12;
                    u32 w0 = (u32)reg.fp[i].exp << 16;
                    u32 w1 = (u32)(reg.fp[i].mantissa >> 32);
                    u32 w2 = (u32)reg.fp[i].mantissa;
                    writeM<C, M, Long>(ea, w0);
                    writeM<C, M, Long>(ea + 4, w1);
                    writeM<C, M, Long>(ea + 8, w2);
                }
                writeA(src, ea);
            } else {
                for (int i = 7; i >= 0; i--) {
                    if (!(regMask & (1 << (7 - i)))) continue;
                    u32 w0 = (u32)reg.fp[i].exp << 16;
                    u32 w1 = (u32)(reg.fp[i].mantissa >> 32);
                    u32 w2 = (u32)reg.fp[i].mantissa;
                    writeM<C, M, Long>(ea, w0);
                    writeM<C, M, Long>(ea + 4, w1);
                    writeM<C, M, Long>(ea + 8, w2);
                    ea += 12;
                }
            }
        }
    }
    else if (cod == 0b100 || cod == 0b101) {
        // FMOVEM system registers — handled by execFMove
        // (some assemblers encode this as FMOVEM instead of FMOVE)
        u32 regList = ___xxx__________ (ext);
        int dst = _____________xxx(opcode);

        if (cod == 0b101) {
            u32 ea, data;
            readOp<C, M, Long>(dst, &ea, &data);
            if (regList & 0b100) reg.fpcr  = data;
            if (regList & 0b010) reg.fpsr  = data;
            if (regList & 0b001) reg.fpiar = data;
        } else {
            u32 data = 0;
            if (regList & 0b100) data = reg.fpcr;
            else if (regList & 0b010) data = reg.fpsr;
            else if (regList & 0b001) data = reg.fpiar;
            writeOp<C, M, Long>(dst, data);
        }
    }

    prefetch<C, POLL>();
    CYCLES_68020(4)
    FINALIZE
}

template <Core C, Instr I, Mode M, Size S> void
Moira::execFGeneric(u16 opcode)
{
    if (!hasFPU()) { execLineF<C, I, M, S>(opcode); return; }
    execFGen<C, I, M, S>(opcode);
    FINALIZE
}
