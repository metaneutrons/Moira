# Moira

A Motorola 68k emulator written in C++20.

Moira is a cycle-exact 68000/68010 emulator with full 68020–68060 instruction support, including FPU (68881/68882/68040), MMU (68040/68060), and internal cache simulation.

## Features

| CPU Model | Integer | FPU | MMU | Cache | Timing |
|-----------|---------|-----|-----|-------|--------|
| 68000 | ✓ | — | — | — | Cycle-exact |
| 68010 | ✓ | — | — | — | Cycle-exact |
| 68020 | ✓ | ✓ (68881/82) | — | — | Functional |
| 68030 | ✓ | ✓ (68881/82) | — | — | Functional |
| 68040 | ✓ | ✓ (hardware) | ✓ | ✓ 4KB I+D | Functional |
| 68060 | ✓ | ✓ (partial¹) | ✓ | ✓ 8KB I+D | Functional |

¹ 68060 FPU traps transcendentals to software (vector 11), matching real hardware behavior.

### FPU (68881/68882/68040/68060)

- All arithmetic instructions with bit-exact results via [Berkeley SoftFloat 3e](https://github.com/ucb-bar/berkeley-softfloat-3)
- Full 64-bit mantissa transcendentals (sin, cos, tan, exp, log, etc.) via polynomial evaluation
- All 7 data formats including Packed BCD with k-factor
- FPCR rounding mode (nearest/zero/−∞/+∞) and precision (extended/single/double)
- FPSR condition codes, exception status, accrued exceptions
- Exception triggering with correct vector numbers (48–54)
- FSAVE/FRESTORE with idle and busy frame formats
- FMOVECR constant ROM (bit-exact Motorola values)
- Cycle-accurate timing (68881/68882 and 68040)

### MMU (68040/68060)

The 68040 and 68060 share the same MMU architecture:

- **ATC** (Address Translation Cache): 4-way set-associative, separate instruction/data
- **TTR** (Transparent Translation Registers): ITT0/ITT1, DTT0/DTT1 with base/mask matching
- **Page table walker**: 3-level for 4K pages, 2-level for 8K pages
- **PFLUSH**: Per-address or global ATC invalidation
- **PTEST**: Page table walk with MMUSR result
- **MOVEC**: TC, ITT0/1, DTT0/1, URP, SRP, MMUSR

### Cache (68040/68060)

- **D-Cache**: Write-back with per-longword dirty tracking, round-robin eviction
- **I-Cache**: Read-only, round-robin eviction
- **68040**: 4KB each (64 sets × 4 ways × 16 bytes)
- **68060**: 8KB each (128 sets × 4 ways × 16 bytes)
- **CINV**: Invalidate by line/page/all (dirty data lost)
- **CPUSH**: Push dirty lines to memory, then invalidate
- **MOVE16**: 16-byte aligned transfer with cache bypass

### 68060-Specific

- PLPA (Physical Load/Push Address)
- LPSTOP (Low-Power Stop)
- PCR, BUSCR registers via MOVEC
- Bus error frame format $4 (FSLW-based, simpler than 68040's format $7)
- FPU transcendentals trap to vector 11 (OS provides FPSP060)
- FMOVEM dynamic register list traps to vector 11

## Integration

Moira is designed as a library. The host application provides memory access via virtual delegates:

```cpp
class MySystem : public moira::Moira {
    u8  read8(u32 addr) const override;
    u16 read16(u32 addr) const override;
    void write8(u32 addr, u8 val) const override;
    void write16(u32 addr, u16 val) const override;
};
```

For 68040/68060, the host calls the MMU/cache methods from its memory delegate:

```cpp
// In your read16 implementation:
auto result = mmu040Translate(addr, false, supervisorMode, true);
if (result.fault) { /* trigger bus error */ }
u32 data = dcache040Read(result.physAddr, result.cacheMode);
```

## Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

## Testing

4863 tests covering FPU, cache, MMU, fuzzing, and edge cases:

```bash
# Build and run the test runner
cd build && cmake --build . && ctest
```

## Documentation

Full documentation at [dirkwhoffmann.github.io/Moira](https://dirkwhoffmann.github.io/Moira)

## License

MIT — see [LICENSE](LICENSE)

Berkeley SoftFloat 3e (vendored in `Moira/softfloat/`) is BSD-licensed.
