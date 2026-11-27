// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Exercises the faithful cMapApp coordinate core (MapScreen::bottomScreenToWorld
// and its inverse). These are static and free of GL/Game, so the test links only
// against mapscreen.cpp. Expected values are computed by hand from the routine at
// 0x793768: X scale +10240 (Q12, = 2.5), Y scale -10240, corner world (-3540, +2520).
#include "mapscreen.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

static int failures;
static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

int main() {
    int32_t w[3];

    // The bottom-screen origin (0,0) with no scroll maps to the map's corner,
    // world (-3540, +2520) in Q12, with z cleared.
    MapScreen::bottomScreenToWorld(0, 0, 0, 0, w);
    check(w[0] == -3540 * 4096 && w[1] == 2520 * 4096 && w[2] == 0,
          "screen origin maps to the map corner, z cleared");

    // One bottom-screen pixel is 2.5 world units: +X moves east, +Y moves south.
    MapScreen::bottomScreenToWorld(4096, 0, 0, 0, w);
    check(w[0] == -3540 * 4096 + 10240 && w[1] == 2520 * 4096,
          "one pixel right is +2.5 world units in X");
    MapScreen::bottomScreenToWorld(0, 4096, 0, 0, w);
    check(w[0] == -3540 * 4096 && w[1] == 2520 * 4096 - 10240,
          "one pixel down is -2.5 world units in Y");

    // The scroll origin (whole pixels) shifts the view before scaling.
    MapScreen::bottomScreenToWorld(0, 0, 1, 0, w);
    check(w[0] == -3540 * 4096 + 10240, "scroll origin X adds one pixel of world offset");
    MapScreen::bottomScreenToWorld(0, 0, 0, 1, w);
    check(w[1] == 2520 * 4096 - 10240, "scroll origin Y adds one pixel of world offset");

    // The 2^44-10240 Y multiplier equals a -10240 scale for the stored 32-bit word.
    MapScreen::bottomScreenToWorld(0, 8192, 0, 0, w);
    check(w[1] == 2520 * 4096 - 2 * 10240, "two pixels down is -5 world units (no overflow drift)");

    // Round-trip: world -> bottom-screen recovers the input to sub-pixel precision.
    const int32_t sxs[] = {0, 4096, 40960, -4096, 123456};
    const int32_t sys[] = {0, 4096, 81920, -8192, -54321};
    const int32_t oxs[] = {0, 3, -5};
    for (int32_t ox : oxs)
        for (int32_t sx : sxs)
            for (int32_t sy : sys) {
                MapScreen::bottomScreenToWorld(sx, sy, ox, ox, w);
                int32_t s[2];
                MapScreen::worldToBottomScreen(w[0], w[1], ox, ox, s);
                check(std::abs(s[0] - sx) <= 4 && std::abs(s[1] - sy) <= 4,
                      "world->screen inverts screen->world within a sub-pixel");
            }

    if (!failures) std::printf("mapscreen_test: all checks passed\n");
    return failures ? 1 : 0;
}
