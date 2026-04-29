// -----------------------------------------------------------------------------
// This file is part of Moira - A Motorola 68k emulator
//
// Copyright (C) Dirk W. Hoffmann. www.dirkwhoffmann.de
// Published under the terms of the MIT License
// -----------------------------------------------------------------------------

#include "MoiraMMU040.h"

namespace moira {

void MMU040::configure(u32 tcr) {
    enabled = (tcr & TCR_ENABLE) != 0;
    page8K = (tcr & TCR_PAGE_8K) != 0;
    pageMask = page8K ? 0x1FFF : 0x0FFF;
    tagMask = page8K ? 0xFFFF0000 : 0xFFFF8000;
}

TTRResult MMU040::matchTTR(u32 ttr, u32 addr, bool super, bool write) const {
    if (!(ttr & 0x8000)) return TTRResult::NoMatch; // bit 15: enabled

    u8 base = (u8)(ttr >> 24);
    u8 mask = (u8)(ttr >> 16);
    u8 addrMSB = (u8)(addr >> 24);

    if ((addrMSB ^ base) & ~mask) return TTRResult::NoMatch;

    // S-field check (bit 14 = enable, bit 13 = supervisor)
    if (ttr & 0x4000) {
        bool ttrSuper = (ttr & 0x2000) != 0;
        if (ttrSuper != super) return TTRResult::NoMatch;
    }

    // Write-protect check (bit 2)
    if (write && (ttr & 0x04)) return TTRResult::WriteProtected;

    return TTRResult::Match;
}

int MMU040::atcIndex(u32 addr) const {
    if (page8K) return (int)((addr >> 13) & 0xF);
    return (int)((addr >> 12) & 0xF);
}

u32 MMU040::atcTag(u32 addr, bool super) const {
    return ((super ? 0x80000000 : 0) | (addr >> 1)) & tagMask;
}

int MMU040::atcLookup(int type, int idx, u32 tag) const {
    for (int w = 0; w < ATC_WAYS; w++) {
        if (atc[type][idx][w].valid && atc[type][idx][w].tag == tag) return w;
    }
    return -1;
}

void MMU040::atcStore(int type, int idx, u32 tag, u32 phys, u16 status) {
    // Find invalid entry or use round-robin eviction
    int way = -1;
    for (int w = 0; w < ATC_WAYS; w++) {
        if (!atc[type][idx][w].valid) { way = w; break; }
    }
    if (way < 0) {
        way = atcEvict[type][idx];
        atcEvict[type][idx] = (atcEvict[type][idx] + 1) & (ATC_WAYS - 1);
    }

    atc[type][idx][way].tag = tag;
    atc[type][idx][way].phys = phys;
    atc[type][idx][way].status = status;
    atc[type][idx][way].valid = true;
}

void MMU040::atcFlush(u32 addr, bool super, bool global) {
    for (int type = 0; type < 2; type++) {
        int idx = atcIndex(addr);
        u32 tag = atcTag(addr, super);
        for (int w = 0; w < ATC_WAYS; w++) {
            auto &e = atc[type][idx][w];
            if (!e.valid) continue;
            if (e.tag != tag) continue;
            if (!global && (e.status & MMUSR_G)) continue;
            e.valid = false;
        }
    }
}

void MMU040::atcFlushAll(bool global) {
    for (int type = 0; type < 2; type++) {
        for (int idx = 0; idx < ATC_SETS; idx++) {
            for (int w = 0; w < ATC_WAYS; w++) {
                auto &e = atc[type][idx][w];
                if (!e.valid) continue;
                if (!global && (e.status & MMUSR_G)) continue;
                e.valid = false;
            }
        }
    }
}

} // namespace moira
