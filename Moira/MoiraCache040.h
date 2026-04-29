// -----------------------------------------------------------------------------
// This file is part of Moira - A Motorola 68k emulator
//
// Copyright (C) Dirk W. Hoffmann. www.dirkwhoffmann.de
// Published under the terms of the MIT License
// -----------------------------------------------------------------------------
//
// 68040 internal cache simulation
// Clean-room implementation based on MC68040 User's Manual (Motorola, 1993)
// -----------------------------------------------------------------------------

#pragma once

#include "MoiraTypes.h"

namespace moira {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int CACHE040_SETS      = 64;
static constexpr int CACHE040_WAYS      = 4;
static constexpr int CACHE040_LINE_SIZE = 16;   // bytes
static constexpr int CACHE040_LINE_LONGS = 4;   // longwords per line

static constexpr u32 CACHE040_TAG_MASK   = 0xFFFFFC00; // upper 22 bits
static constexpr int CACHE040_INDEX_SHIFT = 4;
static constexpr u32 CACHE040_INDEX_MASK  = 0x3F;      // 6 bits (64 sets)

// CACR bits (68040)
static constexpr u32 CACR040_DE = 0x80000000;  // Data cache enable
static constexpr u32 CACR040_NAD = 0x40000000; // No allocate (data)
static constexpr u32 CACR040_IE = 0x00008000;  // Instruction cache enable

// Cache mode (from TTR/page descriptor CM field)
enum class CacheMode : u8 {
    WriteThrough    = 0,    // CM=00: cacheable, write-through
    CopyBack        = 1,    // CM=01: cacheable, copy-back
    NonCacheSerial  = 2,    // CM=10: non-cacheable, serialized
    NonCacheable    = 3     // CM=11: non-cacheable
};

// ---------------------------------------------------------------------------
// Cache line
// ---------------------------------------------------------------------------

struct CacheLine040 {
    u32  tag = 0;                       // Physical address tag
    u32  data[CACHE040_LINE_LONGS] = {};// 4 longwords
    bool dirty[CACHE040_LINE_LONGS] = {};// Per-longword dirty
    bool valid = false;

    bool anyDirty() const { return dirty[0] || dirty[1] || dirty[2] || dirty[3]; }
    void invalidate() { valid = false; }
    void clearDirty() { dirty[0] = dirty[1] = dirty[2] = dirty[3] = false; }
};

// ---------------------------------------------------------------------------
// Cache set (4-way)
// ---------------------------------------------------------------------------

struct CacheSet040 {
    CacheLine040 line[CACHE040_WAYS];
    u8 nextEvict = 0;   // Round-robin eviction pointer
};

// ---------------------------------------------------------------------------
// Full cache (64 sets × 4 ways)
// ---------------------------------------------------------------------------

struct Cache040 {

    CacheSet040 set[CACHE040_SETS];

    // Compute set index from physical address
    static int index(u32 physAddr) {
        return (int)((physAddr >> CACHE040_INDEX_SHIFT) & CACHE040_INDEX_MASK);
    }

    // Compute tag from physical address
    static u32 tag(u32 physAddr) {
        return physAddr & CACHE040_TAG_MASK;
    }

    // Compute longword slot within line
    static int slot(u32 physAddr) {
        return (int)((physAddr >> 2) & 3);
    }

    // Find a line matching the given tag in a set. Returns way index or -1.
    int findLine(int setIdx, u32 tag) const;

    // Invalidate all lines
    void invalidateAll();

    // Invalidate lines matching a physical address range
    void invalidateLine(u32 physAddr);
    void invalidatePage(u32 physAddr, u32 pageSize);
};

} // namespace moira
