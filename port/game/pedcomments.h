// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Ped-comment (cPedComment) selection helpers, split out of the Sound layer so the pure selection/gating logic
// can be unit-tested without the audio backend. Only selection logic and sample ids are modelled here; the voice
// samples and any subtitle text are the user's own run-time game data, looked up by id, never reproduced.
#pragma once
#include "lookups.h"
#include <algorithm>

namespace pedcomment {

// cPedComment::IsCommentInRecentMemory (0x6b4900): a chosen sample is rejected when it is still held in the
// recent-memory ring - the 2-entry "small" ring when the category has fewer than five samples, otherwise the
// 4-entry "large" ring (native globals at 0xe009b0 / 0xe009c0) - or when it is already being spoken by one of
// the (up to two) active comment voices. Returns true when the sample must be rejected.
inline bool isCommentInRecentMemory(const PedCommentTables& tables, int sample, bool small,
                                    const int* activeSamples, int activeCount) {
    const int32_t* ring = small ? tables.recentSmall : tables.recentLarge;
    const int ringCount = small ? 2 : 4;
    if (std::find(ring, ring + ringCount, sample) != ring + ringCount) return true;
    for (int i = 0; i < activeCount; ++i)
        if (activeSamples[i] == sample) return true;
    return false;
}

// cPedComment::StoreCommentInRecentMemory (0x6b4794): push the accepted sample into the small (2-entry, cursor at
// native cPedComment +0x74) or large (4-entry, +0x70) recent-memory ring, advancing that ring's cursor first with
// wrap-around (0<->1 for small, 0..3 for large; native csinc wrap). The cursors live in the owning Sound instance.
inline void storeCommentInRecentMemory(PedCommentTables& tables, int& smallCursor, int& largeCursor,
                                       int sample, bool small) {
    if (small) {
        smallCursor = smallCursor == 1 ? 0 : smallCursor + 1;
        tables.recentSmall[smallCursor] = sample;
    } else {
        largeCursor = largeCursor == 3 ? 0 : largeCursor + 1;
        tables.recentLarge[largeCursor] = sample;
    }
}

} // namespace pedcomment
