// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
// Ped-comment recent-memory gating (cPedComment::IsCommentInRecentMemory 0x6b4900 /
// StoreCommentInRecentMemory 0x6b4794) on synthetic data. No voice samples or subtitle text are involved: these
// exercise only the ring-buffer cursor wrap and the reject-if-recent/active decision over arbitrary sample ids.
#include "pedcomments.h"
#include <cstdio>

static int failures;
static void check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; }
}

static void recentMemoryTests() {
    PedCommentTables t{};
    // A fresh (zeroed) table rejects id 0 because the rings are full of zeros; a non-zero id is free.
    check(!pedcomment::isCommentInRecentMemory(t, 42, true, nullptr, 0),
          "an unseen sample is not in recent memory");
    // Small ring (categories with < 5 samples) is 2 entries; large is 4 and is consulted separately.
    t.recentSmall[0] = 7;
    t.recentSmall[1] = 8;
    check(pedcomment::isCommentInRecentMemory(t, 7, true, nullptr, 0), "small ring rejects a stored sample");
    check(pedcomment::isCommentInRecentMemory(t, 8, true, nullptr, 0), "small ring rejects its other slot");
    check(!pedcomment::isCommentInRecentMemory(t, 7, false, nullptr, 0),
          "the large path does not see the small ring");
    t.recentLarge[2] = 55;
    check(pedcomment::isCommentInRecentMemory(t, 55, false, nullptr, 0), "large ring rejects a stored sample");
    check(!pedcomment::isCommentInRecentMemory(t, 55, true, nullptr, 0),
          "the small path does not see the large ring");

    // An id currently being spoken by an active voice is rejected regardless of the rings.
    const int active[2] = {100, 101};
    check(pedcomment::isCommentInRecentMemory(t, 100, true, active, 2), "an active voice's sample is rejected");
    check(pedcomment::isCommentInRecentMemory(t, 101, false, active, 2),
          "active voices are checked on both ring paths");
    check(!pedcomment::isCommentInRecentMemory(t, 102, true, active, 2),
          "an id no voice is speaking and no ring holds is free");
    check(!pedcomment::isCommentInRecentMemory(t, 101, true, active, 1),
          "only the first activeCount voices are consulted");
}

static void storeWrapTests() {
    // Small ring: cursor starts at 0, pre-increments with 0<->1 wrap, so writes land at 1, 0, 1, 0, ...
    PedCommentTables t{};
    int small = 0, large = 0;
    pedcomment::storeCommentInRecentMemory(t, small, large, 11, true);
    check(small == 1 && t.recentSmall[1] == 11, "first small store lands at index 1");
    pedcomment::storeCommentInRecentMemory(t, small, large, 12, true);
    check(small == 0 && t.recentSmall[0] == 12, "second small store wraps to index 0");
    pedcomment::storeCommentInRecentMemory(t, small, large, 13, true);
    check(small == 1 && t.recentSmall[1] == 13, "third small store returns to index 1");
    check(large == 0, "small stores leave the large cursor untouched");

    // Large ring: cursor starts at 0, wraps 0..3, so writes land at 1, 2, 3, 0, 1, ...
    const int expect[] = {1, 2, 3, 0, 1};
    for (int i = 0; i < 5; ++i) {
        pedcomment::storeCommentInRecentMemory(t, small, large, 200 + i, false);
        check(large == expect[i] && t.recentLarge[large] == 200 + i, "large store advances with 0..3 wrap");
    }

    // Stored ids are then found by the recent-memory check (round trip through both helpers).
    check(pedcomment::isCommentInRecentMemory(t, 13, true, nullptr, 0), "a freshly stored small id reads back");
    check(pedcomment::isCommentInRecentMemory(t, 204, false, nullptr, 0), "a freshly stored large id reads back");
}

int main() {
    recentMemoryTests();
    storeWrapTests();
    if (failures) { std::fprintf(stderr, "%d ped-comment check(s) failed\n", failures); return 1; }
    std::printf("ped comments OK\n");
    return 0;
}
