// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
#include "maplegend.h"
#include <cstdio>

using namespace maplegend;

static int failures;
static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

int main() {
    // --- GetDistanceString numeric pipeline (0x798cf8) ---
    int32_t o[3] = {0, 0, 0};
    int32_t one[3] = {4096, 0, 0};       // 1.0 unit on x
    int32_t ten[3] = {40960, 0, 0};      // 10.0 units on x
    check(mapDistanceNumber(o, o) == 0, "zero distance prints 0");
    check(mapDistanceNumber(o, one) == 1, "one fixed unit prints 1 after the magic scale");
    check(mapDistanceNumber(o, ten) == 19, "ten fixed units print 19 (0x347E00000 then *2457>>24)");
    // symmetry and 3D accumulation
    check(mapDistanceNumber(ten, o) == 19, "distance is order-independent");
    int32_t diag2[3] = {0, 40960, 0};
    check(mapDistanceNumber(ten, diag2) > 19, "the diagonal of two 10-unit legs exceeds a single leg");
    check(mapDistanceString(o, one, " ft") == std::string("1 ft"), "string appends the unit suffix");

    // --- GetDealerSpriteAndPalette drug switch (0x790140..0x7901bc) ---
    check(mapDealerDrugSprite(0x448) == 0x15 && mapDealerDrugSprite(0x44a) == 0x1c &&
          mapDealerDrugSprite(0x44c) == 0x19 && mapDealerDrugSprite(0x44e) == 0x16 &&
          mapDealerDrugSprite(0x450) == 0x18 && mapDealerDrugSprite(0x452) == 0x1b,
          "each known drug id maps to its map sprite");
    check(mapDealerDrugSprite(0x447) == 0x17 && mapDealerDrugSprite(0x999) == 0x17,
          "unknown drug ids fall back to the default dot");

    uint16_t s1, s2; uint32_t p1, p2;
    DealerTip tip;
    tip.drugId = 0x448; tip.flag4 = 0; tip.flag7 = 0; tip.hasDealerBase = true;
    dealerSpriteAndPalette(tip, s1, p1, s2, p2);
    check(s1 == 0x15 && p1 == 9 && s2 == 0x1d && p2 == 5,
          "unvisited dealer: drug sprite + palette 9, secondary 0x1d/5");
    tip.flag4 = 1;
    dealerSpriteAndPalette(tip, s1, p1, s2, p2);
    check(s1 == 0x15 && p1 == 9 && s2 == 0x1a && p2 == 11,
          "visited dealer flips the secondary pair to 0x1a/11");
    // plain path: flag7 != 0 and flagA == 0 -> w24 == 0, dot on both pairs
    DealerTip plain; plain.flag7 = 1; plain.flagA = 0;
    dealerSpriteAndPalette(plain, s1, p1, s2, p2);
    check(s1 == 0x17 && p1 == 9 && s2 == 0x17 && p2 == 9, "plain tip stays the default dot");
    // not found
    check(!mapBlipSprite(false, tip, s1, p1, s2, p2) && s1 == 0x17 && p1 == 9 && s2 == 0x17 && p2 == 9,
          "a missing dealer returns false with the default dot pair");

    // --- DisplayMapLegend hit test (+/-5.0 box, 0x7997d4..0x799840) ---
    LegendBlip blips[2];
    blips[0].x = 100 * 4096; blips[0].y = 100 * 4096; blips[0].legendStringId = 0x540;
    blips[1].x = 200 * 4096; blips[1].y = 200 * 4096; blips[1].legendStringId = 0x549;
    int32_t id = -1;
    int32_t inside[3] = {103 * 4096, 98 * 4096, 0};  // within 5.0 of blip 0
    check(mapLegendHitTest(inside, blips, 2, id) && id == 0x540, "cursor over a blip returns its legend id");
    int32_t edge[3] = {105 * 4096, 100 * 4096, 0};   // exactly +5.0 -> excluded (strict <)
    check(!mapLegendHitTest(edge, blips, 2, id), "the box edge is exclusive like the original compare");
    int32_t outside[3] = {150 * 4096, 150 * 4096, 0};
    check(!mapLegendHitTest(outside, blips, 2, id), "cursor between blips hits nothing");
    int32_t onSecond[3] = {200 * 4096, 203 * 4096, 0};
    check(mapLegendHitTest(onSecond, blips, 2, id) && id == 0x549, "the last matched blip's id wins");

    if (failures == 0) std::printf("maplegend_test: all checks passed\n");
    return failures ? 1 : 0;
}
