// -----------------------------------------------------------------------------
// This file is part of Moira - A Motorola 68k emulator
//
// Copyright (C) Dirk W. Hoffmann. www.dirkwhoffmann.de
// Published under the terms of the MIT License
// -----------------------------------------------------------------------------
//
// 68040 memory subsystem: D-cache read/write/push, I-cache fetch, MMU translate
// Clean-room implementation based on MC68040 User's Manual (Motorola, 1993)
// -----------------------------------------------------------------------------

#include "Moira.h"

namespace moira {

// ---------------------------------------------------------------------------
// D-Cache: Push a dirty line to memory
// ---------------------------------------------------------------------------

void Moira::dcache040Push(int setIdx, int way) {
    auto &line = dcache040.set[setIdx].line[way];
    if (!line.valid || !line.anyDirty()) return;

    u32 baseAddr = line.tag | ((u32)setIdx << CACHE040_INDEX_SHIFT);
    for (int i = 0; i < CACHE040_LINE_LONGS; i++) {
        if (line.dirty[i]) {
            u32 addr = baseAddr | ((u32)i << 2);
            write16(addr, (u16)(line.data[i] >> 16));
            write16(addr + 2, (u16)line.data[i]);
        }
    }
    line.clearDirty();
}

void Moira::dcache040PushLine(u32 physAddr) {
    int idx = dcache040.index(physAddr);
    u32 t = dcache040.tag(physAddr);
    int w = dcache040.findLine(idx, t);
    if (w >= 0) dcache040Push(idx, w);
}

// ---------------------------------------------------------------------------
// D-Cache: Read a longword
// ---------------------------------------------------------------------------

u32 Moira::dcache040Read(u32 physAddr, CacheMode cm) {
    // Non-cacheable: bypass
    if (cm >= CacheMode::NonCacheSerial || !(reg.cacr & CACR040_DE)) {
        return ((u32)read16(physAddr) << 16) | read16(physAddr + 2);
    }

    int idx = dcache040.index(physAddr);
    u32 t = dcache040.tag(physAddr);
    int slot = Cache040::slot(physAddr);

    // Cache lookup
    int w = dcache040.findLine(idx, t);
    if (w >= 0) return dcache040.set[idx].line[w].data[slot]; // Hit

    // Miss: check if allocation is disabled
    if (reg.cacr & CACR040_NAD) {
        return ((u32)read16(physAddr) << 16) | read16(physAddr + 2);
    }

    // Allocate: evict victim, push if dirty, fill from memory
    auto &set = dcache040.set[idx];
    w = set.nextEvict;
    set.nextEvict = (set.nextEvict + 1) & (CACHE040_WAYS - 1);

    if (set.line[w].valid && set.line[w].anyDirty()) {
        dcache040Push(idx, w);
    }

    // Fill line (4 longwords from aligned address)
    u32 lineBase = physAddr & ~0xF;
    set.line[w].tag = t;
    set.line[w].valid = true;
    set.line[w].clearDirty();
    for (int i = 0; i < CACHE040_LINE_LONGS; i++) {
        u32 a = lineBase | ((u32)i << 2);
        set.line[w].data[i] = ((u32)read16(a) << 16) | read16(a + 2);
    }

    return set.line[w].data[slot];
}

// ---------------------------------------------------------------------------
// D-Cache: Write a longword (mask selects which bytes)
// ---------------------------------------------------------------------------

void Moira::dcache040Write(u32 physAddr, u32 val, u32 mask, CacheMode cm) {
    // Non-cacheable: write through
    if (cm >= CacheMode::NonCacheSerial || !(reg.cacr & CACR040_DE)) {
        if (mask == 0xFFFFFFFF) {
            write16(physAddr, (u16)(val >> 16));
            write16(physAddr + 2, (u16)val);
        } else {
            u32 old = ((u32)read16(physAddr) << 16) | read16(physAddr + 2);
            val = (old & ~mask) | (val & mask);
            write16(physAddr, (u16)(val >> 16));
            write16(physAddr + 2, (u16)val);
        }
        return;
    }

    int idx = dcache040.index(physAddr);
    u32 t = dcache040.tag(physAddr);
    int slot = Cache040::slot(physAddr);
    int w = dcache040.findLine(idx, t);

    if (w >= 0) {
        // Hit
        auto &line = dcache040.set[idx].line[w];
        line.data[slot] = (line.data[slot] & ~mask) | (val & mask);
        line.dirty[slot] = true;

        if (cm == CacheMode::WriteThrough) {
            // Write-through: immediately push this longword
            u32 addr = (line.tag | ((u32)idx << CACHE040_INDEX_SHIFT)) | ((u32)slot << 2);
            write16(addr, (u16)(line.data[slot] >> 16));
            write16(addr + 2, (u16)line.data[slot]);
            line.dirty[slot] = false;
        }
        return;
    }

    // Miss
    if (cm == CacheMode::WriteThrough || (reg.cacr & CACR040_NAD)) {
        // Write-through miss or no-allocate: write directly, no allocation
        u32 old = ((u32)read16(physAddr) << 16) | read16(physAddr + 2);
        val = (old & ~mask) | (val & mask);
        write16(physAddr, (u16)(val >> 16));
        write16(physAddr + 2, (u16)val);
        return;
    }

    // Copy-back miss: allocate line, fill, then update
    auto &set = dcache040.set[idx];
    w = set.nextEvict;
    set.nextEvict = (set.nextEvict + 1) & (CACHE040_WAYS - 1);

    if (set.line[w].valid && set.line[w].anyDirty()) {
        dcache040Push(idx, w);
    }

    u32 lineBase = physAddr & ~0xF;
    set.line[w].tag = t;
    set.line[w].valid = true;
    set.line[w].clearDirty();
    for (int i = 0; i < CACHE040_LINE_LONGS; i++) {
        u32 a = lineBase | ((u32)i << 2);
        set.line[w].data[i] = ((u32)read16(a) << 16) | read16(a + 2);
    }
    set.line[w].data[slot] = (set.line[w].data[slot] & ~mask) | (val & mask);
    set.line[w].dirty[slot] = true;
}

// ---------------------------------------------------------------------------
// I-Cache: Fetch a longword
// ---------------------------------------------------------------------------

u32 Moira::icache040Fetch(u32 physAddr) {
    if (!(reg.cacr & CACR040_IE)) {
        return ((u32)read16(physAddr) << 16) | read16(physAddr + 2);
    }

    int idx = icache040.index(physAddr);
    u32 t = icache040.tag(physAddr);
    int slot = Cache040::slot(physAddr);

    int w = icache040.findLine(idx, t);
    if (w >= 0) return icache040.set[idx].line[w].data[slot]; // Hit

    // Miss: allocate (I-cache is read-only, no dirty push needed)
    auto &set = icache040.set[idx];
    w = set.nextEvict;
    set.nextEvict = (set.nextEvict + 1) & (CACHE040_WAYS - 1);

    u32 lineBase = physAddr & ~0xF;
    set.line[w].tag = t;
    set.line[w].valid = true;
    for (int i = 0; i < CACHE040_LINE_LONGS; i++) {
        u32 a = lineBase | ((u32)i << 2);
        set.line[w].data[i] = ((u32)read16(a) << 16) | read16(a + 2);
    }

    return set.line[w].data[slot];
}

// ---------------------------------------------------------------------------
// MMU: Cache mode extraction from TTR
// ---------------------------------------------------------------------------

CacheMode Moira::mmu040CacheModeFromTTR(u32 ttr) const {
    return (CacheMode)((ttr >> 5) & 3);
}

// ---------------------------------------------------------------------------
// MMU: Address translation
// ---------------------------------------------------------------------------

TranslateResult Moira::mmu040Translate(u32 logAddr, bool write, bool super, bool data) {
    TranslateResult result = { logAddr, CacheMode::CopyBack, false, false };

    // If MMU disabled, identity mapping
    if (!mmu040.enabled) return result;

    // Check TTRs (transparent translation)
    u32 ttr0 = data ? reg.dtt0 : reg.itt0;
    u32 ttr1 = data ? reg.dtt1 : reg.itt1;

    TTRResult tr = mmu040.matchTTR(ttr0, logAddr, super, write);
    if (tr == TTRResult::NoMatch) tr = mmu040.matchTTR(ttr1, logAddr, super, write);

    if (tr == TTRResult::Match) {
        result.cacheMode = mmu040CacheModeFromTTR(ttr0); // Use matching TTR's CM
        return result;
    }
    if (tr == TTRResult::WriteProtected) {
        result.fault = true;
        result.writeProtect = true;
        return result;
    }

    // ATC lookup
    int type = data ? ATC_DATA : ATC_INSTRUCTION;
    int idx = mmu040.atcIndex(logAddr);
    u32 tag = mmu040.atcTag(logAddr, super);
    int way = mmu040.atcLookup(type, idx, tag);

    if (way >= 0) {
        auto &e = mmu040.atc[type][idx][way];
        // Check access permissions
        if (write && (e.status & MMUSR_W)) { result.fault = true; result.writeProtect = true; return result; }
        if (!super && (e.status & MMUSR_S)) { result.fault = true; return result; }
        if (!(e.status & MMUSR_R)) { result.fault = true; return result; }

        result.physAddr = e.phys | (logAddr & mmu040.pageMask);
        result.cacheMode = (CacheMode)((e.status >> MMUSR_CM_SHIFT) & 3);
        return result;
    }

    // ATC miss: page table walk
    u32 physOut;
    u16 statusOut;
    if (!mmu040PageWalk(logAddr, write, super, physOut, statusOut)) {
        result.fault = true;
        return result;
    }

    // Store in ATC
    mmu040.atcStore(type, idx, tag, physOut & ~mmu040.pageMask, statusOut);

    result.physAddr = physOut | (logAddr & mmu040.pageMask);
    result.cacheMode = (CacheMode)((statusOut >> MMUSR_CM_SHIFT) & 3);
    return result;
}

// ---------------------------------------------------------------------------
// MMU: Page table walk (3-level for 4K, 2-level effective for 8K)
// ---------------------------------------------------------------------------

bool Moira::mmu040PageWalk(u32 logAddr, bool write, bool super, u32 &physOut, u16 &statusOut) {
    u32 rootPtr = super ? reg.srp : reg.urp;
    bool wp = false;
    statusOut = MMUSR_R; // Assume resident

    // Level 1: Root table descriptor
    u32 l1Index, l2Index, l3Index;
    if (mmu040.page8K) {
        l1Index = (logAddr >> 25) & 0x7F;
        l2Index = (logAddr >> 19) & 0x3F;
        l3Index = (logAddr >> 13) & 0x1F;
    } else {
        l1Index = (logAddr >> 25) & 0x7F;
        l2Index = (logAddr >> 18) & 0x7F;
        l3Index = (logAddr >> 12) & 0x3F;
    }

    // Read root descriptor
    u32 descAddr = (rootPtr & 0xFFFFFE00) | (l1Index << 2);
    u32 desc = ((u32)read16(descAddr) << 16) | read16(descAddr + 2);

    if ((desc & 3) == PD_INVALID) { statusOut = 0; return false; }
    if (desc & PD_WRITEPROTECT) wp = true;

    // Level 2: Pointer table descriptor
    descAddr = (desc & 0xFFFFFE00) | (l2Index << 2);
    desc = ((u32)read16(descAddr) << 16) | read16(descAddr + 2);

    if ((desc & 3) == PD_INVALID) { statusOut = 0; return false; }
    if (desc & PD_WRITEPROTECT) wp = true;

    // Level 3: Page descriptor
    u32 pageMask = mmu040.page8K ? 0xFFFFFF80 : 0xFFFFFF00;
    descAddr = (desc & pageMask) | (l3Index << 2);
    desc = ((u32)read16(descAddr) << 16) | read16(descAddr + 2);

    u32 type = desc & 3;
    if (type == PD_INVALID) { statusOut = 0; return false; }

    // Handle indirect descriptor
    if (type == PD_INDIRECT) {
        descAddr = desc & 0xFFFFFFFC;
        desc = ((u32)read16(descAddr) << 16) | read16(descAddr + 2);
        type = desc & 3;
        if (type == PD_INVALID) { statusOut = 0; return false; }
    }

    if (desc & PD_WRITEPROTECT) wp = true;

    // Extract physical address
    physOut = desc & ~mmu040.pageMask;

    // Build status
    statusOut = MMUSR_R;
    if (wp) statusOut |= MMUSR_W;
    if (desc & PD_MODIFIED) statusOut |= MMUSR_M;
    if (desc & PD_GLOBAL) statusOut |= MMUSR_G;
    if (desc & PD_SUPER) statusOut |= MMUSR_S;
    statusOut |= (u16)(((desc >> 5) & 3) << MMUSR_CM_SHIFT); // CM field

    // Set Used bit in descriptor (write back)
    if (!(desc & PD_USED)) {
        desc |= PD_USED;
        write16(descAddr, (u16)(desc >> 16));
        write16(descAddr + 2, (u16)desc);
    }

    // Set Modified bit if write access
    if (write && !(desc & PD_MODIFIED)) {
        if (wp) { statusOut = MMUSR_W; return false; } // Write-protect fault
        desc |= PD_MODIFIED;
        write16(descAddr, (u16)(desc >> 16));
        write16(descAddr + 2, (u16)desc);
        statusOut |= MMUSR_M;
    }

    return true;
}

} // namespace moira
