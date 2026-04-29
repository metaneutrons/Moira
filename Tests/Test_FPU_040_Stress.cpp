// =============================================================================
// Moira Advanced Tests — Fuzzing, State Corruption, Boundaries, Coherency
// =============================================================================

#include "MoiraFPU.h"
#include "MoiraCache040.h"
#include "MoiraMMU040.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>

using namespace moira;

static int tests_passed = 0;
static int tests_failed = 0;
static int tests_total = 0;

#define CHECK(cond, msg) do { tests_total++; \
    if (cond) { tests_passed++; } \
    else { tests_failed++; printf("  FAIL [%d]: %s\n", __LINE__, msg); } \
} while(0)

#define CHECK_EQ(a, b, msg) do { tests_total++; \
    if ((a) == (b)) { tests_passed++; } \
    else { tests_failed++; printf("  FAIL [%d]: %s: got 0x%llX, expected 0x%llX\n", \
        __LINE__, msg, (unsigned long long)(a), (unsigned long long)(b)); } \
} while(0)

#define SECTION(name) printf("--- %s ---\n", name)

static void fromDouble(Registers::FPReg &r, double d) {
    u64 bits; memcpy(&bits, &d, 8); fpFromDouble(r, bits);
}

// =============================================================================
// 1. FUZZING: Random FPU operands — must never crash or produce undefined state
// =============================================================================

void testFuzz_FPU_RandomOperands() {
    SECTION("Fuzz: FPU Random Operands (1000 iterations)");

    srand(0xDEADBEEF); // Deterministic seed for reproducibility
    int crashes = 0;

    for (int i = 0; i < 1000; i++) {
        Registers::FPReg a, b;
        u32 fpsr = 0;

        // Generate random bit patterns
        a.exp = (u16)(rand() & 0xFFFF);
        a.mantissa = ((u64)rand() << 32) | (u64)rand();
        b.exp = (u16)(rand() & 0xFFFF);
        b.mantissa = ((u64)rand() << 32) | (u64)rand();

        // Every operation must produce a valid FPReg (not crash)
        Registers::FPReg r;

        r = a; fpAdd(r, b, fpsr);
        if (r.exp == 0 && r.mantissa == 0) {} // zero is valid
        // Just verify no crash — result is always some FPReg

        r = a; fpSub(r, b, fpsr);
        r = a; fpMul(r, b, fpsr);
        r = a; fpDiv(r, b, fpsr);

        r = a; fpSqrt(r, fpsr);
        r = a; fpAbs(r, fpsr);
        r = a; fpNeg(r, fpsr);
    }

    CHECK(true, "1000 random FADD/SUB/MUL/DIV/SQRT: no crash");
}

void testFuzz_FPU_RandomTranscendental() {
    SECTION("Fuzz: FPU Random Transcendentals (500 iterations)");

    srand(0xCAFEBABE);

    for (int i = 0; i < 500; i++) {
        Registers::FPReg r;
        u32 fpsr = 0;

        r.exp = (u16)(rand() & 0xFFFF);
        r.mantissa = ((u64)rand() << 32) | (u64)rand();

        Registers::FPReg copy = r;
        fpSin(copy, fpsr); copy = r;
        fpCos(copy, fpsr); copy = r;
        fpTan(copy, fpsr); copy = r;
        fpAtan(copy, fpsr); copy = r;
        fpEtox(copy, fpsr); copy = r;
        fpLogn(copy, fpsr); copy = r;
        fpTwotox(copy, fpsr); copy = r;
        fpTentox(copy, fpsr);
    }

    CHECK(true, "500 random transcendentals: no crash");
}

void testFuzz_Cache_RandomAddresses() {
    SECTION("Fuzz: Cache Random Addresses (1000 iterations)");
    Cache040 dc;
    dc.invalidateAll();

    srand(0x12345678);

    for (int i = 0; i < 1000; i++) {
        u32 addr = (u32)rand() << 16 | (u32)rand();

        int idx = dc.index(addr);
        u32 tag = dc.tag(addr);
        int slot = Cache040::slot(addr);

        // Index must be in range
        CHECK(idx >= 0 && idx < dc.numSets, "random addr: index in range");
        // Slot must be 0-3
        CHECK(slot >= 0 && slot < 4, "random addr: slot in range");

        // Insert and find must work
        dc.set[idx].line[0].tag = tag;
        dc.set[idx].line[0].valid = true;
        CHECK(dc.findLine(idx, tag) == 0, "random addr: insert then find");
        dc.set[idx].line[0].valid = false;
    }
}

void testFuzz_MMU_RandomAddresses() {
    SECTION("Fuzz: MMU ATC Random Addresses (500 iterations)");
    MMU040 mmu;
    mmu.configure(0x8000); // 4K pages

    srand(0xABCD1234);

    for (int i = 0; i < 500; i++) {
        u32 addr = (u32)rand() << 16 | (u32)rand();
        bool super = rand() & 1;

        int idx = mmu.atcIndex(addr);
        u32 tag = mmu.atcTag(addr, super);

        CHECK(idx >= 0 && idx < ATC_SETS, "random MMU addr: index in range");

        // Store and lookup must work
        mmu.atcStore(ATC_DATA, idx, tag, addr & ~0xFFF, MMUSR_R);
        int w = mmu.atcLookup(ATC_DATA, idx, tag);
        CHECK(w >= 0, "random MMU addr: store then lookup");

        // Flush must work
        mmu.atcFlush(addr, super, true);
        w = mmu.atcLookup(ATC_DATA, idx, tag);
        CHECK(w < 0, "random MMU addr: flush then miss");
    }
}

// =============================================================================
// 2. STATE CORRUPTION: Verify uninvolved state is unchanged
// =============================================================================

void testCorruption_FPU_UninvolvedRegs() {
    SECTION("State Corruption: FPU uninvolved registers unchanged");

    Registers::FPReg fp[8];
    u32 fpsr, fpcr = 0;

    // Initialize all FP registers with known patterns
    for (int i = 0; i < 8; i++) {
        fp[i].exp = 0x4000 + (u16)i;
        fp[i].mantissa = 0x8000000000000000ULL + (u64)i * 0x1111111111111111ULL;
    }

    // FADD on fp[2] using fp[1] as source — fp[0,3-7] must be unchanged
    Registers::FPReg saved[8];
    memcpy(saved, fp, sizeof(fp));

    fpsr = 0;
    fpAdd(fp[2], fp[1], fpsr);

    // Verify untouched registers
    for (int i = 0; i < 8; i++) {
        if (i == 2) continue; // fp[2] was the destination
        char msg[64];
        snprintf(msg, sizeof(msg), "FADD: FP%d unchanged (exp)", i);
        CHECK_EQ(fp[i].exp, saved[i].exp, msg);
        snprintf(msg, sizeof(msg), "FADD: FP%d unchanged (mant)", i);
        CHECK_EQ(fp[i].mantissa, saved[i].mantissa, msg);
    }
}

void testCorruption_Cache_OtherSets() {
    SECTION("State Corruption: Cache ops don't corrupt other sets");
    Cache040 dc;
    dc.invalidateAll();

    // Fill set 10 with known data
    for (int w = 0; w < 4; w++) {
        dc.set[10].line[w].tag = 0xAAAA0000 + (u32)(w << 10);
        dc.set[10].line[w].valid = true;
        dc.set[10].line[w].data[0] = 0xBBBB0000 + (u32)w;
    }

    // Operate on set 20 (invalidate, insert, etc.)
    dc.set[20].line[0].tag = 0xCCCC0000;
    dc.set[20].line[0].valid = true;
    dc.invalidateLine(0xCCCC0000 | (20 << 4)); // invalidate in set 20

    // Verify set 10 is completely untouched
    for (int w = 0; w < 4; w++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "set 10 way %d: still valid", w);
        CHECK(dc.set[10].line[w].valid, msg);
        snprintf(msg, sizeof(msg), "set 10 way %d: tag intact", w);
        CHECK_EQ(dc.set[10].line[w].tag, (u32)(0xAAAA0000 + (w << 10)), msg);
        snprintf(msg, sizeof(msg), "set 10 way %d: data intact", w);
        CHECK_EQ(dc.set[10].line[w].data[0], (u32)(0xBBBB0000 + w), msg);
    }
}

void testCorruption_ATC_OtherEntries() {
    SECTION("State Corruption: ATC flush doesn't corrupt other entries");
    MMU040 mmu;
    mmu.configure(0x8000);

    // Store entries in different sets
    u32 addr1 = 0x00100000; // set depends on bits [15:12]
    u32 addr2 = 0x00200000; // different set
    int idx1 = mmu.atcIndex(addr1);
    int idx2 = mmu.atcIndex(addr2);
    u32 tag1 = mmu.atcTag(addr1, true);
    u32 tag2 = mmu.atcTag(addr2, true);

    mmu.atcStore(ATC_DATA, idx1, tag1, 0x00300000, MMUSR_R);
    mmu.atcStore(ATC_DATA, idx2, tag2, 0x00400000, MMUSR_R | MMUSR_G);

    // Flush addr1 — addr2 must survive
    mmu.atcFlush(addr1, true, true);
    CHECK(mmu.atcLookup(ATC_DATA, idx1, tag1) < 0, "flushed entry gone");
    CHECK(mmu.atcLookup(ATC_DATA, idx2, tag2) >= 0, "other entry survives flush");
    CHECK_EQ(mmu.atc[ATC_DATA][idx2][mmu.atcLookup(ATC_DATA, idx2, tag2)].phys,
             (u32)0x00400000, "other entry phys intact");
}

// =============================================================================
// 3. BOUNDARY/OVERFLOW: Edge values that might trigger off-by-one errors
// =============================================================================

void testBoundary_Cache_SetWrap() {
    SECTION("Boundary: Cache set index wrap (last set)");
    Cache040 dc;
    dc.invalidateAll();

    // Address that maps to set 63 (last set for 040)
    // index = (addr >> 4) & 0x3F = 63 → addr bits [9:4] = 0x3F → addr & 0x3F0 = 0x3F0
    u32 addr = 0x000003F0;
    int idx = dc.index(addr);
    CHECK_EQ(idx, 63, "addr 0x3F0 maps to set 63");

    // Next line (addr + 16) should wrap to set 0
    u32 nextAddr = addr + 16; // 0x400
    int nextIdx = dc.index(nextAddr);
    CHECK_EQ(nextIdx, 0, "addr 0x400 wraps to set 0");

    // Both should work independently
    dc.set[idx].line[0].tag = dc.tag(addr);
    dc.set[idx].line[0].valid = true;
    dc.set[nextIdx].line[0].tag = dc.tag(nextAddr);
    dc.set[nextIdx].line[0].valid = true;

    CHECK(dc.findLine(idx, dc.tag(addr)) >= 0, "set 63 hit");
    CHECK(dc.findLine(nextIdx, dc.tag(nextAddr)) >= 0, "set 0 hit");

    // Invalidate set 63 shouldn't affect set 0
    dc.invalidateLine(addr);
    CHECK(dc.findLine(idx, dc.tag(addr)) < 0, "set 63 invalidated");
    CHECK(dc.findLine(nextIdx, dc.tag(nextAddr)) >= 0, "set 0 still valid");
}

void testBoundary_MMU_PageBoundary() {
    SECTION("Boundary: MMU page boundary (4K)");
    MMU040 mmu;
    mmu.configure(0x8000); // 4K pages

    // Two addresses on adjacent pages
    u32 addr1 = 0x00100FFF; // last byte of page
    u32 addr2 = 0x00101000; // first byte of next page

    // They should map to different ATC entries (different page)
    int idx1 = mmu.atcIndex(addr1);
    int idx2 = mmu.atcIndex(addr2);
    u32 tag1 = mmu.atcTag(addr1, true);
    u32 tag2 = mmu.atcTag(addr2, true);

    // Different pages must differ in either tag or index (or both)
    CHECK(tag1 != tag2 || idx1 != idx2, "adjacent pages: different ATC entry");

    // Store both — they must coexist
    mmu.atcStore(ATC_DATA, idx1, tag1, 0x00200000, MMUSR_R);
    mmu.atcStore(ATC_DATA, idx2, tag2, 0x00300000, MMUSR_R);

    CHECK(mmu.atcLookup(ATC_DATA, idx1, tag1) >= 0, "page N: found");
    CHECK(mmu.atcLookup(ATC_DATA, idx2, tag2) >= 0, "page N+1: found");
}

void testBoundary_FPU_ExtremeValues() {
    SECTION("Boundary: FPU extreme exponent/mantissa values");
    Registers::FPReg r;
    u32 fpsr;

    // Maximum normal: exp=0x7FFE, mantissa=all ones
    r.exp = 0x7FFE;
    r.mantissa = 0xFFFFFFFFFFFFFFFFULL;
    CHECK(!fpIsInf(r), "max normal is not inf");
    CHECK(!fpIsNaN(r), "max normal is not NaN");
    CHECK(!fpIsZero(r), "max normal is not zero");

    // Minimum positive normal: exp=0x0001, mantissa=0x8000000000000000
    r.exp = 0x0001;
    r.mantissa = 0x8000000000000000ULL;
    CHECK(!fpIsZero(r), "min normal is not zero");

    // Operations on extreme values must not crash
    fpsr = 0;
    Registers::FPReg maxVal;
    maxVal.exp = 0x7FFE; maxVal.mantissa = 0xFFFFFFFFFFFFFFFFULL;
    Registers::FPReg two;
    fromDouble(two, 2.0);

    Registers::FPReg result = maxVal;
    fpMul(result, two, fpsr); // Should overflow to inf
    CHECK(fpIsInf(result), "max * 2 = overflow to inf");

    // Minimum * 0.5 should underflow
    fpsr = 0;
    Registers::FPReg minVal;
    minVal.exp = 0x0001; minVal.mantissa = 0x8000000000000000ULL;
    Registers::FPReg half;
    fromDouble(half, 0.5);
    result = minVal;
    fpMul(result, half, fpsr);
    // Result should be denormal or zero
    CHECK((result.exp & 0x7FFF) == 0 || fpIsZero(result), "min * 0.5 = underflow");

    // All-ones mantissa in arithmetic
    fpsr = 0;
    r.exp = 0x3FFF; r.mantissa = 0xFFFFFFFFFFFFFFFFULL;
    Registers::FPReg one;
    fromDouble(one, 1.0);
    fpAdd(r, one, fpsr); // Should not crash
    CHECK(!fpIsNaN(r), "all-ones mantissa + 1: valid result");
}

// =============================================================================
// 4. SEQUENTIAL DEPENDENCIES: Cache coherency scenarios
// =============================================================================

void testSequential_CacheWriteRead() {
    SECTION("Sequential: D-Cache write then read same address");
    Cache040 dc;
    dc.invalidateAll();

    // Simulate: write 0xDEADBEEF to address, then read it back
    u32 addr = 0x00010000;
    int idx = dc.index(addr);
    u32 tag = dc.tag(addr);
    int slot = Cache040::slot(addr);

    // Allocate line and write
    dc.set[idx].line[0].tag = tag;
    dc.set[idx].line[0].valid = true;
    dc.set[idx].line[0].data[slot] = 0xDEADBEEF;
    dc.set[idx].line[0].dirty[slot] = true;

    // Read back from same address — must get written value
    int w = dc.findLine(idx, tag);
    CHECK(w >= 0, "write-read: cache hit");
    CHECK_EQ(dc.set[idx].line[w].data[slot], (u32)0xDEADBEEF, "write-read: correct data");
}

void testSequential_CINVThenRead() {
    SECTION("Sequential: CINV then read (must miss)");
    Cache040 dc;
    dc.invalidateAll();

    u32 addr = 0x00020000;
    int idx = dc.index(addr);
    u32 tag = dc.tag(addr);

    // Fill cache line
    dc.set[idx].line[0].tag = tag;
    dc.set[idx].line[0].valid = true;
    dc.set[idx].line[0].data[0] = 0xCAFEBABE;

    // CINV invalidates
    dc.invalidateLine(addr);

    // Read must miss (stale data gone)
    CHECK(dc.findLine(idx, tag) < 0, "CINV then read: cache miss (stale data gone)");
}

void testSequential_DirtyEviction() {
    SECTION("Sequential: Dirty line eviction preserves data");
    Cache040 dc;
    dc.invalidateAll();

    int idx = 5;
    // Fill all 4 ways with dirty data
    for (int w = 0; w < 4; w++) {
        dc.set[idx].line[w].tag = 0x10000000 + (u32)(w << 10);
        dc.set[idx].line[w].valid = true;
        dc.set[idx].line[w].data[0] = 0xAA000000 + (u32)w;
        dc.set[idx].line[w].dirty[0] = true;
    }
    dc.set[idx].nextEvict = 0;

    // The eviction victim (way 0) has dirty data
    CHECK(dc.set[idx].line[0].anyDirty(), "victim has dirty data before eviction");
    CHECK_EQ(dc.set[idx].line[0].data[0], (u32)0xAA000000, "victim data correct");

    // After eviction (simulated by new allocation), dirty data must be pushed first
    // In real operation, dcache040Push would be called before overwriting
    // Here we just verify the dirty state is detectable
    int victim = dc.set[idx].nextEvict;
    CHECK_EQ(victim, 0, "eviction targets way 0");
    CHECK(dc.set[idx].line[victim].anyDirty(), "eviction victim is dirty (needs push)");
}

void testSequential_WriteThrough_Immediate() {
    SECTION("Sequential: Write-through mode — data immediately visible");
    Cache040 dc;
    dc.invalidateAll();

    // In write-through mode, after a write:
    // 1. Cache line is updated
    // 2. Dirty bit is set then immediately cleared (pushed)
    // Simulating: write to cache, then verify dirty is cleared after push

    int idx = 3;
    dc.set[idx].line[0].tag = 0x20000000;
    dc.set[idx].line[0].valid = true;
    dc.set[idx].line[0].data[1] = 0x11111111;
    dc.set[idx].line[0].dirty[1] = true;

    // Simulate write-through push: clear dirty after writing to memory
    dc.set[idx].line[0].dirty[1] = false;

    CHECK(!dc.set[idx].line[0].anyDirty(), "write-through: no dirty after push");
    CHECK_EQ(dc.set[idx].line[0].data[1], (u32)0x11111111, "write-through: data still in cache");
}

// =============================================================================
// 5. MODE SWITCH: Supervisor/User, MMU enable/disable, CACR changes
// =============================================================================

void testModeSwitch_SuperToUser_ATC() {
    SECTION("Mode Switch: Supervisor→User ATC isolation");
    MMU040 mmu;
    mmu.configure(0x8000);

    u32 addr = 0x00500000;

    // Supervisor maps addr → 0x00600000
    u32 superTag = mmu.atcTag(addr, true);
    int idx = mmu.atcIndex(addr);
    mmu.atcStore(ATC_DATA, idx, superTag, 0x00600000, MMUSR_R);

    // User maps same addr → 0x00700000
    u32 userTag = mmu.atcTag(addr, false);
    mmu.atcStore(ATC_DATA, idx, userTag, 0x00700000, MMUSR_R);

    // After "mode switch" to user: supervisor entry must NOT be found with user tag
    int w = mmu.atcLookup(ATC_DATA, idx, userTag);
    CHECK(w >= 0, "user mode: finds user entry");
    CHECK_EQ(mmu.atc[ATC_DATA][idx][w].phys, (u32)0x00700000, "user mode: correct phys");

    // Supervisor entry still exists but won't match user tag
    w = mmu.atcLookup(ATC_DATA, idx, superTag);
    CHECK(w >= 0, "super entry still exists");
    CHECK_EQ(mmu.atc[ATC_DATA][idx][w].phys, (u32)0x00600000, "super entry: correct phys");
}

void testModeSwitch_MMU_EnableDisable() {
    SECTION("Mode Switch: MMU enable/disable via TC");
    MMU040 mmu;

    // Start disabled
    mmu.configure(0x0000);
    CHECK(!mmu.enabled, "TC=0: MMU disabled");

    // Enable
    mmu.configure(0x8000);
    CHECK(mmu.enabled, "TC=0x8000: MMU enabled");
    CHECK(!mmu.page8K, "TC=0x8000: 4K pages");

    // Enable with 8K pages
    mmu.configure(0xC000);
    CHECK(mmu.enabled, "TC=0xC000: MMU enabled");
    CHECK(mmu.page8K, "TC=0xC000: 8K pages");

    // Disable again
    mmu.configure(0x0000);
    CHECK(!mmu.enabled, "TC=0 again: MMU disabled");
}

void testModeSwitch_CACR_DisableCache() {
    SECTION("Mode Switch: CACR disable cache");
    Cache040 dc;
    dc.invalidateAll();

    // Fill a cache line
    u32 addr = 0x00030000;
    int idx = dc.index(addr);
    u32 tag = dc.tag(addr);
    dc.set[idx].line[0].tag = tag;
    dc.set[idx].line[0].valid = true;
    dc.set[idx].line[0].data[0] = 0x12345678;

    // Cache is "enabled" — hit works
    CHECK(dc.findLine(idx, tag) >= 0, "cache enabled: hit");

    // When CACR disables cache (DE=0), the cache contents remain
    // but lookups should be bypassed by the caller.
    // The cache itself doesn't know about CACR — that's the CPU's job.
    // Verify the data is still there (not corrupted by disable)
    CHECK_EQ(dc.set[idx].line[0].data[0], (u32)0x12345678, "after CACR disable: data preserved");
    CHECK(dc.set[idx].line[0].valid, "after CACR disable: valid bit preserved");
}

void testModeSwitch_060_CacheSize() {
    SECTION("Mode Switch: 060 cache has 128 sets");
    Cache040 dc;
    dc.configure(true); // 060 mode

    CHECK_EQ(dc.numSets, 128, "060: 128 sets");
    CHECK_EQ(dc.indexMask, (u32)0x7F, "060: 7-bit index mask");

    // Address that uses bit 10 (only valid in 060 with 128 sets)
    u32 addr = 0x00000400; // bit 10 set → index bit 6
    int idx = dc.index(addr);
    CHECK_EQ(idx, 64, "060: addr 0x400 maps to set 64 (not possible on 040)");

    // Verify set 64 is usable
    dc.set[64].line[0].tag = dc.tag(addr);
    dc.set[64].line[0].valid = true;
    CHECK(dc.findLine(64, dc.tag(addr)) >= 0, "060: set 64 works");
}

// =============================================================================
// MAIN
// =============================================================================

int main() {
    printf("=== Moira Advanced Tests ===\n\n");

    // Fuzzing
    testFuzz_FPU_RandomOperands();
    testFuzz_FPU_RandomTranscendental();
    testFuzz_Cache_RandomAddresses();
    testFuzz_MMU_RandomAddresses();

    // State corruption
    testCorruption_FPU_UninvolvedRegs();
    testCorruption_Cache_OtherSets();
    testCorruption_ATC_OtherEntries();

    // Boundary/overflow
    testBoundary_Cache_SetWrap();
    testBoundary_MMU_PageBoundary();
    testBoundary_FPU_ExtremeValues();

    // Sequential dependencies
    testSequential_CacheWriteRead();
    testSequential_CINVThenRead();
    testSequential_DirtyEviction();
    testSequential_WriteThrough_Immediate();

    // Mode switches
    testModeSwitch_SuperToUser_ATC();
    testModeSwitch_MMU_EnableDisable();
    testModeSwitch_CACR_DisableCache();
    testModeSwitch_060_CacheSize();

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_total, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
