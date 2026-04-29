// -----------------------------------------------------------------------------
// This file is part of Moira - A Motorola 68k emulator
//
// Copyright (C) Dirk W. Hoffmann. www.dirkwhoffmann.de
// Published under the terms of the MIT License
// -----------------------------------------------------------------------------

#include "MoiraCache040.h"

namespace moira {

int Cache040::findLine(int setIdx, u32 t) const {
    auto &s = set[setIdx];
    for (int w = 0; w < CACHE040_WAYS; w++) {
        if (s.line[w].valid && s.line[w].tag == t) return w;
    }
    return -1;
}

void Cache040::invalidateAll() {
    for (int i = 0; i < numSets; i++)
        for (int w = 0; w < CACHE040_WAYS; w++)
            set[i].line[w].invalidate();
}

void Cache040::invalidateLine(u32 physAddr) {
    int idx = index(physAddr);
    u32 t = tag(physAddr);
    int w = findLine(idx, t);
    if (w >= 0) set[idx].line[w].invalidate();
}

void Cache040::invalidatePage(u32 physAddr, u32 pageSize) {
    for (u32 off = 0; off < pageSize; off += CACHE040_LINE_SIZE) {
        invalidateLine(physAddr + off);
    }
}

} // namespace moira
