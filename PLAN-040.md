# Moira 68040 Full Execution Implementation Plan

## Goal
World-class 68040 emulation with internal cache simulation, MMU with ATC and page table walker, and correct bus error frame generation. Clean-room reimplementation based on Motorola specifications.

## Reference Documents (public)
- **MC68040 User's Manual** (Motorola, 1993)
- **M68000 Family Programmer's Reference Manual** (Motorola, 1992)
- WinUAE as behavioral reference (algorithms only, no code copying)

## Architecture Overview

```
┌─────────────────────────────────────────────────────┐
│                    Moira CPU Core                     │
├─────────────────────────────────────────────────────┤
│  Instruction Fetch → I-Cache (64×4, 16B lines)      │
│  Data Read/Write  → D-Cache (64×4, 16B lines, WB)  │
│         ↓                    ↓                       │
│  ┌─────────────────────────────────────────┐        │
│  │   MMU: TTR Check → ATC Lookup → Walk    │        │
│  └─────────────────────────────────────────┘        │
│         ↓                                            │
│  delegate.read/write(physAddr)  → Host Memory        │
└─────────────────────────────────────────────────────┘
```

---

## Phase 1: Cache Infrastructure (Week 1)

### New File: `Moira/MoiraCache040.h`

```cpp
struct CacheLine040 {
    u32 tag;            // Physical address tag (upper 22 bits)
    u32 data[4];       // 4 longwords (16 bytes)
    bool dirty[4];     // Per-longword dirty flags
    bool valid;        // Line valid
};

struct CacheSet040 {
    CacheLine040 line[4];   // 4-way set-associative
    u8 nextEvict;           // Round-robin eviction counter
};

struct Cache040 {
    CacheSet040 set[64];    // 64 sets
    bool enabled;           // CACR enable bit

    // Tag: physAddr & 0xFFFFFC00 (upper 22 bits)
    // Index: (physAddr >> 4) & 0x3F (bits 9:4, 6-bit set index)
    // Offset: physAddr & 0xF (4-bit byte offset within line)
};
```

### Constants
```cpp
static constexpr int CACHE_SETS     = 64;
static constexpr int CACHE_WAYS     = 4;
static constexpr int CACHE_LINE_SIZE = 16;
static constexpr u32 CACHE_TAG_MASK  = 0xFFFFFC00;  // ~((64 << 4) - 1)
static constexpr u32 CACHE_INDEX_MASK = 0x3F;        // 6 bits
static constexpr int CACHE_INDEX_SHIFT = 4;
```

### D-Cache Operations

**Read (dcacheRead):**
1. Compute index = (physAddr >> 4) & 0x3F, tag = physAddr & TAG_MASK
2. Search 4 ways for matching valid tag → hit: return data[offset/4]
3. Miss + cacheable + allocation enabled:
   - Select victim (round-robin), push if dirty, fill 4 longs from memory
   - Return requested data
4. Miss + non-cacheable: read directly from memory via delegate

**Write (dcacheWrite):**
1. Compute index/tag, search for hit
2. Hit + copyback mode: update data, set dirty bit
3. Hit + write-through mode: update data, set dirty, immediately push line
4. Miss + copyback: allocate line (evict+push victim), fill from memory, update, set dirty
5. Miss + write-through: write directly to memory (no allocation)
6. Hit + cache disabled by MMU: push+invalidate line, write to memory

**Push (dcachePush):**
1. For each dirty longword in line: write to memory via delegate
2. Clear dirty flags and gdirty
3. Reconstruct address: tag | (index << 4) | (word << 2)

### I-Cache Operations

**Fetch (icacheFetch):**
1. Compute index/tag from physical address
2. Search 4 ways → hit: return data
3. Miss: fetch 4 longs from memory, allocate line (evict victim — no push needed, I-cache is read-only)
4. Return requested longword

---

## Phase 2: MMU — ATC and Page Table Walker (Week 2)

### New File: `Moira/MoiraMMU040.h`

```cpp
struct ATCEntry {
    u32 tag;        // S-bit | (logical_addr >> 1) masked
    u32 phys;       // Physical page base address
    u32 status;     // G, U1, U0, S, CM, M, W, R bits
    bool valid;
};

struct ATC040 {
    ATCEntry entry[2][16][4];  // [I/D][16 sets][4 ways]
    u8 nextEvict[2][16];       // Per-set round-robin

    // Tag: ((super ? 0x80000000 : 0) | (addr >> 1)) & tagMask
    // Index (4K): (addr >> 12) & 0xF
    // Index (8K): (addr >> 13) & 0xF
};
```

### TTR Matching (Transparent Translation)
```
Input: addr, super, data, write
For each TTR (ITT0/ITT1 for instruction, DTT0/DTT1 for data):
  1. If not enabled (bit 15 = 0): skip
  2. base = TTR[31:24], mask = TTR[23:16]
  3. addrMSB = addr[31:24]
  4. If (addrMSB ^ base) & ~mask != 0: no match, skip
  5. If S-field enabled (bit 14): check bit 13 vs super mode
  6. Match! Extract cache mode (bits 6:5), write-protect (bit 2)
  7. Return: physical = logical (identity mapping), cache mode, WP
```

### ATC Lookup
```
Input: logicalAddr, super, data, write
1. Compute tag = ((super ? 0x80000000 : 0) | (addr >> 1)) & tagMask
2. Compute index based on page size (4K: bits 15:12, 8K: bits 16:13)
3. Search 4 ways in entry[data][index]:
   a. If valid && tag matches:
      - If write && W bit set → access fault
      - If super==false && S bit set → access fault
      - If R bit clear → access fault (page not resident)
      - If write && M bit clear → need to set M bit (re-walk)
      - Return phys | (addr & pageMask), cache mode from CM
   b. Track empty/invalid ways for replacement
4. Miss: call pageTableWalk(), store result, retry
```

### Page Table Walk (3-level for 4K, 2-level for 8K)
```
Input: logicalAddr, super, write
Using: SRP (super) or URP (user) as root pointer

4K pages:
  Level 1 (Root): index = (addr >> 25) & 0x7F, desc_addr = (root & 0xFFFFFE00) | (index << 2)
  Level 2 (Pointer): index = (addr >> 18) & 0x7F, desc_addr = (desc & 0xFFFFFE00) | (index << 2)
  Level 3 (Page): index = (addr >> 12) & 0x3F, desc_addr = (desc & 0xFFFFFF00) | (index << 2)

8K pages:
  Level 1 (Root): index = (addr >> 25) & 0x7F
  Level 2 (Pointer): index = (addr >> 19) & 0x3F
  Level 3 (Page): index = (addr >> 13) & 0x1F

Each level:
  1. Read descriptor from memory (via D-cache, no-allocate mode)
  2. Check type bits [1:0]: 0=invalid→fault, 2=indirect→follow, 1/3=valid
  3. Accumulate write-protect from all levels
  4. At page level: extract physical base, G, U1, U0, S, CM, M bits
  5. If write: set M bit in descriptor (write back to memory)
  6. Set U (Used) bit in descriptor

Return: physical address, status bits, or bus error
```

### PFLUSH
```
PFLUSH (An): flush ATC entries matching addr (non-global only)
PFLUSHN (An): flush ATC entries matching addr (including global)
PFLUSHA: flush all non-global entries
PFLUSHAN: flush all entries
```

### PTEST
```
1. Flush ATC entry for target address
2. Check TTR match → if match: MMUSR = T|R (or B if write-protected)
3. If no TTR match: perform page table walk, populate ATC entry
4. MMUSR = phys_addr[31:12] | status bits
```

---

## Phase 3: CINV / CPUSH / MOVE16 (Week 2-3)

### CINV/CPUSH Decode
```
Opcode: 1111 0100 CC P SSSSS
  CC (bits 7:6): 01=data, 10=instruction, 11=both
  P (bit 5): 0=CINV, 1=CPUSH
  Scope (bits 4:3): 01=line, 10=page, 11=all
  An (bits 2:0): address register (for line/page scope)
```

### CINV Execution
```
1. SUPERVISOR_MODE_ONLY
2. Decode cache, scope, register
3. Switch on scope:
   - Line: addr = An & ~0xF; invalidate matching line in selected cache(s)
   - Page: for each line in page (4K/8K step 16): invalidate matching
   - All: invalidate all valid lines in selected cache(s)
4. For D-cache CINV: dirty data is LOST (no write-back)
```

### CPUSH Execution
```
Same as CINV but:
- For D-cache: push (write-back) dirty lines BEFORE invalidating
- For I-cache: just invalidate (no dirty data)
```

### MOVE16 Execution
```
1. SUPERVISOR_MODE_ONLY (actually available in user mode too on 68040)
2. Align source address: srcAddr = An & ~0xF
3. Align destination address: dstAddr = dest & ~0xF
4. Read phase:
   - Translate srcAddr through MMU
   - If D-cache hit: read from cache (no new allocation on miss)
   - Read 4 longwords into transfer buffer
5. Write phase:
   - Translate dstAddr through MMU
   - If D-cache has line for dstAddr: push dirty data, invalidate
   - Write 4 longwords to memory (bypass cache, no allocation)
6. Post-increment: An += 16 (for PI variants)
```

---

## Phase 4: Bus Error Frame Format $7 (Week 3)

### Stack Frame (60 bytes, format $7)
```
SP+0x00: SR
SP+0x02: PC (high word)
SP+0x04: PC (low word)
SP+0x06: Format ($7) | Vector offset
SP+0x08: Effective Address (high)
SP+0x0A: Effective Address (low)
SP+0x0C: SSW
SP+0x0E: WB3S (write-back 3 status)
SP+0x10: WB2S (write-back 2 status)
SP+0x12: WB1S (write-back 1 status)
SP+0x14: Fault Address (high)
SP+0x16: Fault Address (low)
SP+0x18: WB3A (high)
SP+0x1A: WB3A (low)
SP+0x1C: WB3D (high)
SP+0x1E: WB3D (low)
SP+0x20: WB2A (high)
SP+0x22: WB2A (low)
SP+0x24: WB2D (high)
SP+0x26: WB2D (low)
SP+0x28: WB1A (high)
SP+0x2A: WB1A (low)
SP+0x2C: WB1D/PD0 (high)
SP+0x2E: WB1D/PD0 (low)
SP+0x30: PD1 (high)
SP+0x32: PD1 (low)
SP+0x34: PD2 (high)
SP+0x36: PD2 (low)
SP+0x38: PD3 (high)
SP+0x3A: PD3 (low)
```

### SSW Bits
```
Bit 15: CP (continuation pending)
Bit 14: CU (unfinished)
Bit 13: CT (MOVEM in progress)
Bit 12: CM (MOVE16 in progress)
Bit 11: MA (misaligned — not first access of split)
Bit 10: ATC (MMU/ATC fault, not bus error)
Bit  9: LK (locked RMW — CAS/TAS)
Bit  8: RW (1=read, 0=write)
Bits 6:5: SIZE (00=long, 01=byte, 10=word, 11=line)
Bits 4:3: TT (transfer type)
Bits 2:0: TM (transfer mode / function code)
```

### Write-Back Buffer
```cpp
struct WriteBackEntry {
    u32 addr;
    u32 data;
    u16 status;     // bit 7 = valid, bits 6:0 = size|TT|TM
};

// In Registers:
WriteBackEntry wb[3];       // 3 write-back entries
u32 faultAddr;              // Logical address that faulted
u32 move16Data[4];          // MOVE16 transfer buffer
u16 ssw;                    // Computed SSW
```

---

## Phase 5: MOVEC Extensions (Week 3-4)

### New 68040 Control Registers
| Code | Name | Description |
|------|------|-------------|
| $003 | TC | Translation Control (bit 15=enable, bit 14=page size) |
| $004 | ITT0 | Instruction Transparent Translation 0 |
| $005 | ITT1 | Instruction Transparent Translation 1 |
| $006 | DTT0 | Data Transparent Translation 0 |
| $007 | DTT1 | Data Transparent Translation 1 |
| $805 | MMUSR | MMU Status Register (read-only via MOVEC) |
| $806 | URP | User Root Pointer |
| $807 | SRP | Supervisor Root Pointer |

### Register Additions to `Registers` struct
```cpp
u32 tc = 0;         // Translation Control
u32 itt0 = 0;       // Instruction TTR 0
u32 itt1 = 0;       // Instruction TTR 1
u32 dtt0 = 0;       // Data TTR 0
u32 dtt1 = 0;       // Data TTR 1
u32 urp = 0;        // User Root Pointer
u32 srp = 0;        // Supervisor Root Pointer
u32 mmusr = 0;      // MMU Status Register
```

### CACR Mask for 68040
```cpp
// Only bits 31 (DE) and 15 (IE) are writable on 68040
static constexpr u32 CACR_040_MASK = 0x80008000;
```

---

## Phase 6: Integration and Memory Access Path (Week 4)

### Modified Memory Access Flow
For 68040 model, all memory accesses go through:
```
1. TTR check (identity mapping if match)
2. ATC lookup (fast path)
3. Page table walk on ATC miss (slow path)
4. Cache lookup/fill (D-cache for data, I-cache for instructions)
5. Physical memory access via delegate (on cache miss)
```

### Delegate Changes
```cpp
// Existing (unchanged):
virtual u8  read8(u32 addr);
virtual u16 read16(u32 addr);
virtual u32 read32(u32 addr);
virtual void write8(u32 addr, u8 val);
virtual void write16(u32 addr, u16 val);
virtual void write32(u32 addr, u32 val);

// New (for MMU page table reads):
// Uses existing delegates — page table is in physical memory
// No new delegates needed; the walker calls read32() directly
```

### Performance Consideration
The cache/MMU adds overhead to every memory access. For performance:
- Fast path: TTR match → identity map → cache hit (no translation needed)
- ATC hit: single array lookup + cache check
- Only on ATC miss: expensive page table walk

---

## File Structure

```
Moira/
├── MoiraCache040.h         # Cache data structures and inline helpers
├── MoiraCache040.cpp       # Cache operations (read/write/push/invalidate)
├── MoiraMMU040.h           # ATC, TTR, page walker declarations
├── MoiraMMU040.cpp         # MMU operations (translate, walk, flush)
├── MoiraExec_cpp.h         # CINV, CPUSH, MOVE16 execution (modified)
├── MoiraExecMMU_cpp.h      # PFLUSH40, PTEST40 execution (modified)
├── MoiraExceptions_cpp.h   # Bus error frame $7 (modified)
├── MoiraTypes.h            # Register additions (modified)
├── Moira.h                 # MMU/cache state, new methods (modified)
├── MoiraInit_cpp.h         # Verify instruction registration (modified)
```

---

## Testing Strategy

### Unit Tests
- Cache: fill/evict/push/invalidate with known patterns
- ATC: lookup/miss/fill/flush with known page tables
- TTR: matching logic with various address ranges
- Page walker: 3-level walk with constructed page tables in memory
- MOVE16: all 5 variants with alignment verification
- Bus error frame: verify exact byte layout matches Motorola spec

### Integration Tests
- `simple_040.hdf.zip` from Cputester (integer instructions)
- `FBASIC_040.hdf.zip` from Cputester (FPU on 040)
- WinUAE golden vectors: run instruction sequences, capture register/memory state

### What We Do NOT Test
- MMU instructions (Cputester explicitly excludes them: "Not going to happen")
- Cache coherency edge cases (board-specific behavior)
- MOVE16 unaligned behavior (documented as "board-specific" in Cputester)

---

## Implementation Order

1. **Cache040** — data structures + D-cache read/write/push + I-cache fetch
2. **MMU040** — TTR matching + ATC lookup + page table walker
3. **CINV/CPUSH** — uses cache infrastructure
4. **MOVE16** — uses cache + MMU
5. **PFLUSH/PTEST** — uses ATC
6. **Bus error frame $7** — uses write-back buffer state
7. **MOVEC extensions** — register access
8. **Integration** — wire MMU into memory access path for 040 models
9. **Testing** — golden vectors + unit tests
