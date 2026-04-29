# Moira FPU Implementation Plan

## Goal
Implement 68881/68882/68040 FPU execution in Moira, enabling A1200 emulation.
Clean-room implementation based on public Motorola specifications.

## Reference Documents (public, no GPL)
- **M68000 Family Programmer's Reference Manual** (Motorola, 1992) — Section 5: FPU
- **MC68881/MC68882 Floating-Point Coprocessor User's Manual** (Motorola, 1987)
- **IEEE 754-1985** — Floating-point arithmetic standard
- **Berkeley SoftFloat** (BSD license) — 80-bit extended precision library

## Architecture (Moira-Style)

### New Files
```
Moira/
├── MoiraFPU.h              # FPU state: registers, FPCR, FPSR, FPIAR
├── MoiraFPU_cpp.h          # FPU arithmetic (replaces stubs in MoiraExecFPU_cpp.h)
├── softfloat/              # Berkeley SoftFloat (BSD license, vendored)
│   ├── softfloat.h
│   ├── softfloat.cpp
│   └── ...
```

### Modified Files
```
Moira/
├── MoiraTypes.h            # Add FPU registers to Registers struct
├── MoiraExecFPU_cpp.h      # Replace throw stubs with real implementations
├── Moira.h                 # Add FPU state, accessors
```

## Phase 1: FPU Registers & Infrastructure (Week 1)

### 1.1 FPU Register State
Add to `Registers` struct in `MoiraTypes.h`:
```cpp
// FPU Registers (68881/68882/68040)
struct FPRegisters {
    u8  fp[8][12];      // FP0-FP7: 80-bit extended precision (96-bit padded)
    u32 fpcr;           // FP Control Register (rounding mode, precision, exceptions)
    u32 fpsr;           // FP Status Register (condition codes, quotient, exceptions)
    u32 fpiar;          // FP Instruction Address Register
};
```

### 1.2 FPCR Bit Layout (from Motorola manual)
```
FPCR[15:14] = Rounding Precision (00=Extended, 01=Single, 10=Double)
FPCR[13:12] = Rounding Mode (00=Nearest, 01=Zero, 10=-∞, 11=+∞)
FPCR[7:0]   = Exception Enable (BSNAN, SNAN, OPERR, OVFL, UNFL, DZ, INEX2, INEX1)
```

### 1.3 FPSR Bit Layout
```
FPSR[31:28] = Condition Codes (N, Z, I, NAN)
FPSR[27:24] = Quotient byte
FPSR[15:8]  = Exception Status (same as FPCR enables)
FPSR[7:0]   = Accrued Exceptions
```

### 1.4 SoftFloat Integration
- Vendor Berkeley SoftFloat 3e (BSD license) into `Moira/softfloat/`
- Wrapper: `MoiraFPU.h` with Moira-style types
- Key types: `floatx80` (80-bit extended), `float32`, `float64`
- Key ops: add, sub, mul, div, sqrt, comparison, conversion

## Phase 2: Data Transfer Instructions (Week 2-3)

### 2.1 FMOVE (register ↔ memory)
- Source formats: Byte, Word, Long, Single, Double, Extended, Packed BCD
- Destination formats: same
- Format conversion via SoftFloat
- Opcode: `execFGen` with cod=0b000 (reg-to-reg) or cod=0b010 (mem-to-reg) or cod=0b011 (reg-to-mem)

### 2.2 FMOVEM (multiple register transfer)
- Move multiple FP registers to/from memory
- Register list or dynamic register list
- Pre-decrement and post-increment addressing

### 2.3 FMOVECR (move constant ROM)
- 22 predefined constants (π, e, ln2, ln10, powers of 10, etc.)
- Table lookup, return as extended precision
- Opcode map from Motorola manual Table 5-1

### 2.4 FMOVE to/from FPCR/FPSR/FPIAR
- System register access
- Part of FMOVEM encoding

## Phase 3: Arithmetic Instructions (Week 3-5)

### 3.1 Basic Arithmetic
Each follows the pattern: read source → convert to extended → operate → round → store → set FPSR

| Instruction | Opcode | Operation |
|---|---|---|
| FADD | 0x22 | FPn ← FPn + source |
| FSUB | 0x28 | FPn ← FPn - source |
| FMUL | 0x23 | FPn ← FPn × source |
| FDIV | 0x20 | FPn ← FPn ÷ source |
| FSQRT | 0x04 | FPn ← √source |
| FABS | 0x18 | FPn ← |source| |
| FNEG | 0x1A | FPn ← -source |
| FINT | 0x01 | FPn ← int(source) (round per FPCR) |
| FINTRZ | 0x03 | FPn ← int(source) (round to zero) |

### 3.2 Single/Double Precision Variants (68040)
| FSADD/FDADD | 0x62/0x66 | Same as FADD but round to single/double |
| FSSUB/FDSUB | 0x68/0x6C | ... |
| FSMUL/FDMUL | 0x63/0x67 | ... |
| FSDIV/FDDIV | 0x60/0x64 | ... |
| FSABS/FDABS | 0x58/0x5C | ... |
| FSNEG/FDNEG | 0x5A/0x5E | ... |
| FSSQRT/FDSQRT | 0x41/0x45 | ... |

### 3.3 Comparison
| FCMP | 0x38 | Set FPSR condition codes from FPn - source |
| FTST | 0x3A | Set FPSR condition codes from source |

### 3.4 Miscellaneous
| FSGLDIV | 0x24 | Single-precision divide |
| FSGLMUL | 0x27 | Single-precision multiply |
| FMOD | 0x21 | IEEE remainder (sign of dividend) |
| FREM | 0x25 | IEEE remainder (round to nearest) |
| FSCALE | 0x26 | FPn ← FPn × 2^int(source) |
| FGETEXP | 0x1E | Extract exponent |
| FGETMAN | 0x1F | Extract mantissa |

## Phase 4: Transcendental Instructions (Week 5-6)

Implemented via SoftFloat + standard math algorithms (CORDIC or polynomial approximation).
NOT copied from any existing emulator — implemented from IEEE 754 and Motorola specs.

| Instruction | Opcode | Operation |
|---|---|---|
| FSIN | 0x0E | sine |
| FCOS | 0x1D | cosine |
| FSINCOS | 0x30-37 | simultaneous sin+cos |
| FTAN | 0x0F | tangent |
| FASIN | 0x0C | arc sine |
| FACOS | 0x1C | arc cosine |
| FATAN | 0x0A | arc tangent |
| FSINH | 0x02 | hyperbolic sine |
| FCOSH | 0x19 | hyperbolic cosine |
| FTANH | 0x09 | hyperbolic tangent |
| FATANH | 0x0D | hyperbolic arc tangent |
| FETOX | 0x10 | e^x |
| FETOXM1 | 0x08 | e^x - 1 |
| FTWOTOX | 0x11 | 2^x |
| FTENTOX | 0x12 | 10^x |
| FLOGN | 0x14 | ln(x) |
| FLOGNP1 | 0x06 | ln(x+1) |
| FLOG2 | 0x16 | log₂(x) |
| FLOG10 | 0x15 | log₁₀(x) |

## Phase 5: Branch & Control Instructions (Week 6-7)

### 5.1 FBcc (Floating-Point Branch)
- 32 condition codes (EQ, NE, GT, LT, GE, LE, GL, GLE, NGLE, NGE, NGL, NGT, NLE, NLT, OGT, OGE, OLT, OLE, OGL, OR, UN, UEQ, UGT, UGE, ULT, ULE, SNE, ST, SF, SEQ, NGLE, NGL)
- Test FPSR condition codes (N, Z, I, NAN)
- Word and Long displacement

### 5.2 FDBcc (Decrement and Branch)
- Like DBcc but with FPU conditions

### 5.3 FScc (Set on FPU Condition)
- Set byte to 0xFF or 0x00

### 5.4 FTRAPcc (Trap on FPU Condition)

### 5.5 FSAVE / FRESTORE
- Save/restore FPU internal state (for context switching)
- Null frame, idle frame, busy frame
- 68040 format differs from 68881/68882

## Phase 6: Exception Handling & FPSR (Week 7)

- BSNAN: Branch/Set on Signaling NaN
- SNAN: Signaling NaN
- OPERR: Operand Error (e.g., 0/0, ∞-∞)
- OVFL: Overflow
- UNFL: Underflow
- DZ: Divide by Zero
- INEX2: Inexact Result
- INEX1: Inexact Decimal Input

Each arithmetic operation must:
1. Check operands for special values (NaN, ±∞, ±0)
2. Perform operation
3. Round result per FPCR precision and mode
4. Set FPSR exception bits
5. Set FPSR condition codes (N, Z, I, NAN)
6. If exception enabled in FPCR → trigger FPU exception

## Phase 7: Testing (Week 7-8)

### Golden Vector Tests
- Use Motorola's published test vectors (from MC68881/68882 User's Manual appendix)
- Use `cputester` (Toni Wilen's test suite — public domain test vectors)
- Compare against known-good results, NOT against WinUAE code

### Test Categories
1. Basic arithmetic accuracy (all rounding modes)
2. Special values (NaN, ±∞, ±0, denormals)
3. Exception generation
4. FMOVECR constant accuracy
5. Transcendental function accuracy (at least 64-bit mantissa precision)
6. FMOVE format conversions (all 7 formats)
7. FMOVEM register save/restore
8. FBcc condition code evaluation

## Implementation Notes

### Moira Style Rules
- All exec functions follow `template <Core C, Instr I, Mode M, Size S> void` pattern
- Use `AVAILABILITY()` macro for model checks
- Use `CYCLES_68020()` for timing
- Use `delegate.` for memory access
- FPU arithmetic in separate `MoiraFPU_cpp.h` (like `MoiraALU_cpp.h`)
- No external dependencies except SoftFloat (BSD)

### What We Do NOT Copy
- No WinUAE code, variable names, or code structure
- No UAE-specific abstractions (fpdata, fptype, function pointer tables)
- Our implementation uses Moira's template-based dispatch, not UAE's switch-case
- Our SoftFloat integration is direct (not through UAE's abstraction layer)

### SoftFloat License
Berkeley SoftFloat 3e is BSD-licensed — compatible with MIT (Moira) and commercial use.
