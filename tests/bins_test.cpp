// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// cBinBag separable fragments, checked against libGame.so:
//   cBinBag::ClassId        0x0085e834  (mov w0,#0x24)
//   cBinBag::UpdateDynamic  0x0085e83c  settle fragment: decay v by (v*0x8CC)>>12, then settle when
//                                       abs(vx)<=0x3FF && abs(vy)<=0x3FF && abs(vz)<0x2000
// Synthetic velocities only; the live per-frame update needs the shared cDynamicProp object.
#include "bins.h"
#include <cstdio>
#include <cstdlib>

static int failures;
static void check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; } }

int main() {
    // --- ClassId ---
    check(BinBag::ClassId() == 0x24, "ClassId should be 0x24 (36)");

    // Decay = (v * 0x8CC) >> 12, 0x8CC = 2252.
    //  1000 -> (1000*2252)>>12 = 2252000/4096 = 549
    // -1000 -> arithmetic (-2252000)>>12 = floor(-549.80) = -550
    //  4000 -> (4000*2252)>>12 = 9008000/4096 = 2199
    {
        int32_t v[3] = {1000, -1000, 4000};
        bool settled = BinBag::settleStep(v);
        check(v[0] == 549,  "decay 1000 -> 549");
        check(v[1] == -550, "decay -1000 -> -550");
        check(v[2] == 2199, "decay 4000 -> 2199");
        // 549<=0x3FF(1023), 550<=1023, 2199<0x2000(8192) -> settles
        check(settled, "small velocity should settle");
    }

    // 5000 -> (5000*2252)>>12 = 11260000/4096 = 2749; 2749 > 1023 -> does not settle
    {
        int32_t v[3] = {5000, 0, 0};
        bool settled = BinBag::settleStep(v);
        check(v[0] == 2749, "decay 5000 -> 2749");
        check(!settled, "fast horizontal velocity should not settle");
    }

    // z boundary: decayed vz must be strictly < 0x2000 (8192) to settle.
    //  14899 -> (14899*2252)>>12 = 33552548/4096 = 8191  (settles)
    //  14900 -> (14900*2252)>>12 = 33554800/4096 = 8192  (does NOT settle: 8192 is not < 8192)
    {
        int32_t lo[3] = {0, 0, 14899};
        bool s1 = BinBag::settleStep(lo);
        check(lo[2] == 8191, "decay 14899 -> 8191");
        check(s1, "vz just below threshold should settle");

        int32_t hi[3] = {0, 0, 14900};
        bool s2 = BinBag::settleStep(hi);
        check(hi[2] == 8192, "decay 14900 -> 8192");
        check(!s2, "vz at threshold 0x2000 should not settle");
    }

    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("all bin-bag checks passed\n");
    return 0;
}
