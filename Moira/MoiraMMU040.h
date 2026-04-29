// -----------------------------------------------------------------------------
// This file is part of Moira - A Motorola 68k emulator
//
// Copyright (C) Dirk W. Hoffmann. www.dirkwhoffmann.de
// Published under the terms of the MIT License
// -----------------------------------------------------------------------------
//
// 68040 MMU: ATC (Address Translation Cache) and page table walker
// Clean-room implementation based on MC68040 User's Manual (Motorola, 1993)
// -----------------------------------------------------------------------------

#pragma once

#include "MoiraTypes.h"
#include "MoiraCache040.h"

namespace moira {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int ATC_SETS = 16;
static constexpr int ATC_WAYS = 4;

// ATC type indices
static constexpr int ATC_INSTRUCTION = 0;
static constexpr int ATC_DATA        = 1;

// TCR bits
static constexpr u32 TCR_ENABLE    = 0x8000;  // bit 15: MMU enable
static constexpr u32 TCR_PAGE_8K   = 0x4000;  // bit 14: 0=4K, 1=8K

// Page descriptor type bits [1:0]
static constexpr u32 PD_INVALID    = 0;
static constexpr u32 PD_RESIDENT   = 1;
static constexpr u32 PD_INDIRECT   = 2;
static constexpr u32 PD_RESIDENT2  = 3;

// Page descriptor status bits
static constexpr u32 PD_WRITEPROTECT = 0x04;  // bit 2: write-protected
static constexpr u32 PD_USED         = 0x08;  // bit 3: used (accessed)
static constexpr u32 PD_MODIFIED     = 0x10;  // bit 4: modified (written)
static constexpr u32 PD_GLOBAL       = 0x400; // bit 10: global
static constexpr u32 PD_SUPER        = 0x80;  // bit 7: supervisor only

// MMUSR bits
static constexpr u32 MMUSR_R = 0x001;  // Resident
static constexpr u32 MMUSR_T = 0x002;  // Transparent (TTR match)
static constexpr u32 MMUSR_W = 0x004;  // Write-protected
static constexpr u32 MMUSR_M = 0x008;  // Modified
static constexpr u32 MMUSR_CM_SHIFT = 4;// Cache mode (2 bits)
static constexpr u32 MMUSR_S = 0x040;  // Supervisor protected
static constexpr u32 MMUSR_U0 = 0x080; // User page attribute 0
static constexpr u32 MMUSR_U1 = 0x100; // User page attribute 1
static constexpr u32 MMUSR_G = 0x200;  // Global
static constexpr u32 MMUSR_B = 0x400;  // Bus error during walk

// TTR matching results
enum class TTRResult { NoMatch, Match, WriteProtected };

// Translation result
struct TranslateResult {
    u32 physAddr;
    CacheMode cacheMode;
    bool fault;         // Access fault occurred
    bool writeProtect;  // Page is write-protected
};

// ---------------------------------------------------------------------------
// ATC Entry
// ---------------------------------------------------------------------------

struct ATCEntry {
    u32  tag = 0;       // S-bit(31) | (logical >> 1) masked
    u32  phys = 0;      // Physical page base
    u16  status = 0;    // G, U1, U0, S, CM, M, W, R packed
    bool valid = false;
};

// ---------------------------------------------------------------------------
// MMU040 — full MMU state
// ---------------------------------------------------------------------------

struct MMU040 {

    // ATC: [I/D][16 sets][4 ways]
    ATCEntry atc[2][ATC_SETS][ATC_WAYS];
    u8 atcEvict[2][ATC_SETS] = {};

    // Derived from TCR
    bool enabled = false;
    bool page8K = false;
    u32 pageMask = 0xFFF;       // 0xFFF for 4K, 0x1FFF for 8K
    u32 tagMask = 0xFFFF8000;   // depends on page size

    // Update derived fields from TCR value
    void configure(u32 tcr);

    // TTR matching
    TTRResult matchTTR(u32 ttr, u32 addr, bool super, bool write) const;

    // ATC operations
    int atcIndex(u32 addr) const;
    u32 atcTag(u32 addr, bool super) const;
    int atcLookup(int type, int idx, u32 tag) const;
    void atcStore(int type, int idx, u32 tag, u32 phys, u16 status);
    void atcFlush(u32 addr, bool super, bool global);
    void atcFlushAll(bool global);
};

} // namespace moira
