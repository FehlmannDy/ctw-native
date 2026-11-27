// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Faithful cMapApp map helpers. See maplegend.h. Each routine is matched against
// libGame.so; addresses in comments cite the original instruction, never its text.
#include "maplegend.h"

#include <cmath>
#include <cstdint>

#include "hud.h"

namespace maplegend {

// cFixed<20,12>: one real unit == 4096 raw. The legend hit box is +/-5.0.
static constexpr int32_t kFixedOne = 4096;
static constexpr int32_t kLegendHalfBox = 5 * kFixedOne;  // 0x5000 (#5,lsl#12)

// ---- cMapApp::GetDealerSpriteAndPalette (0x790038) -------------------------
// Drug id -> sprite id switch, 0x790140..0x7901bc (observation). Default 0x17.
uint16_t mapDealerDrugSprite(uint16_t drugId) {
    switch (drugId) {
        case 0x448: return 0x15;  // 0x7901a0
        case 0x44a: return 0x1c;  // 0x7901b0
        case 0x44c: return 0x19;  // 0x790160
        case 0x44e: return 0x16;  // 0x7901a8
        case 0x450: return 0x18;  // 0x7901b8
        case 0x452: return 0x1b;  // 0x79018c
        default:    return 0x17;  // 0x7900c8 / fallthrough
    }
}

void dealerSpriteAndPalette(const DealerTip& tip,
                            uint16_t& sprite1, uint32_t& palette1,
                            uint16_t& sprite2, uint32_t& palette2) {
    // 0x790110: record +0x07 == 0 -> drug path; else if +0x0A == 0 -> plain dot.
    bool drugPath;
    int w24 = 0;         // original w24: 1 = derived-pair, 0 = plain
    uint16_t w25 = 0x17; // original w25: the primary sprite id
    if (tip.flag7 == 0) {
        drugPath = true;
    } else if (tip.flagA == 0) {
        // 0x790194: w24 = 0, w25 = 0x17
        drugPath = false;
        w24 = 0;
        w25 = 0x17;
    } else {
        drugPath = true;
    }

    if (drugPath) {
        if (!tip.hasDealerBase) {
            // 0x790168: no dealer base -> w24 = 1, w25 = 0x17
            w24 = 1;
            w25 = 0x17;
        } else {
            // 0x790134: read drug id, map to sprite; w24 = 1
            w24 = 1;
            w25 = mapDealerDrugSprite(tip.drugId);
        }
    }

    // 0x7901bc..0x790204: assemble the two pairs.
    sprite1 = w25;          // strh w25,[x23]
    palette1 = 9;           // str  #9,[x22]  (always 9)

    // record +0x04 selects the "visited" variant.
    uint32_t palA = (tip.flag4 == 0) ? 5u : 11u;    // csel w9
    uint16_t sprA = (tip.flag4 == 0) ? 0x1d : 0x1a; // csel w8

    if (w24 != 0) {
        sprite2 = sprA;     // csel w8, ..., ne
        palette2 = palA;    // csel w9, ..., ne
    } else {
        sprite2 = w25;      // csel w8 keeps w25
        palette2 = 9;       // csel w9 keeps 9
    }
}

bool mapBlipSprite(bool found, const DealerTip& tip,
                   uint16_t& sprite1, uint32_t& palette1,
                   uint16_t& sprite2, uint32_t& palette2) {
    if (!found) {
        // 0x7900c4: not found -> dot 0x17 / palette 9 on both pairs, return -1.
        sprite1 = 0x17;
        palette1 = 9;
        sprite2 = 0x17;
        palette2 = 9;
        return false;
    }
    dealerSpriteAndPalette(tip, sprite1, palette1, sprite2, palette2);
    return true;
}

// ---- cMapApp::GetDistanceString (0x798cf8) ---------------------------------
int32_t mapDistanceNumber(const int32_t a[3], const int32_t b[3]) {
    // 0x798d1c..0x798d40: squared 3D distance in raw^2 units (int64 accumulate).
    int64_t dx = (int64_t)a[0] - b[0];
    int64_t dy = (int64_t)a[1] - b[1];
    int64_t dz = (int64_t)a[2] - b[2];
    uint64_t sumSq = (uint64_t)(dx * dx + dy * dy + dz * dz);

    // 0x798d4c..0x798d54: unsigned convert, sqrt, truncate to uint32 (raw fixed).
    double d = std::sqrt((double)sumSq);
    uint32_t distRaw = (uint32_t)d;  // fcvtzu

    // 0x798d58..0x798d70: magic scale to the printed integer.
    //   t = (distRaw * 0x347E00000) >> 32 ; n = ((int32)t * 2457) >> 12 >> 12
    int64_t x8 = (int64_t)(int32_t)distRaw * 0x347E00000LL;  // sxtw + mul
    x8 >>= 32;                                               // asr #0x20
    int64_t p = (int64_t)(int32_t)x8 * 2457;                 // smull w8, #0x999
    uint64_t pu = (uint64_t)p >> 12;                         // lsr #0xC
    return (int32_t)pu >> 12;                                // asr w3, #0xC
}

std::string mapDistanceString(const int32_t a[3], const int32_t b[3],
                              const std::string& unitSuffix) {
    // 0x798d74..0x798d9c: format the number, convert to unicode, append the unit
    // GXT string (id 0x5A). The suffix is supplied by the caller (game text data).
    return std::to_string(mapDistanceNumber(a, b)) + unitSuffix;
}

// ---- cMapApp::DisplayMapLegend (0x799728) ----------------------------------
bool mapLegendHitTest(const int32_t cursor[3], const LegendBlip* blips, int count,
                      int32_t& outStringId) {
    // 0x7997d4..0x799840: a blip is "hovered" when the cursor is inside its
    // +/-5.0 (fixed) axis-aligned box. The original keeps the LAST matched blip's
    // +0x0C field (csel w8) and ORs the hit flags; we mirror that exactly.
    const int32_t cx = cursor[0];
    const int32_t cy = cursor[1];
    bool any = false;
    int32_t matched = 0;  // original w8 starts at 0
    for (int i = 0; i < count; ++i) {
        const LegendBlip& b = blips[i];
        // x range: [x-5, x+5] (0x7997f8..0x799808)
        if (!(b.x - kLegendHalfBox < cx && cx < b.x + kLegendHalfBox))
            continue;
        // y range: [y-5, y+5] (0x79981c..0x799830)
        bool inY = (b.y - kLegendHalfBox < cy) && (cy < b.y + kLegendHalfBox);
        if (inY) {
            matched = b.legendStringId;  // csel w8, w13, w8, ne
            any = true;                  // orr w10
        }
    }
    outStringId = matched;
    return any;
}

uint16_t mapLegendStringForType(int type, int subType, bool& resolved) {
    (void)type;
    (void)subType;
    // 0x7998c0..0x799a6c maps a blip type (0..0x74) through a 117-entry byte jump
    // table at 0x483C04 to a case that sets the GXT id (0x539..0x55B). That table
    // is extracted game data and is deliberately not reproduced here, so the
    // type->id mapping cannot be resolved offline. The in-code sub-type (w23)
    // branches that disambiguate a few cases are documented in the report; without
    // the table we cannot know which types reach them. Callers that already hold
    // the resolved GXT id (e.g. from the game's own table at runtime) pass it to
    // drawMapLegend directly.
    resolved = false;
    return 0;
}

void drawMapLegend(int screenW, int screenH, uint16_t sprite, uint8_t palette,
                   const std::u16string& label) {
    // The original routes the (radar sprite, GXT string) pair through
    // cPdaHelpManager::ForceDisplayHelpTip (0x7998a0), whose panel geometry is not
    // part of 0x799728. This port-side layout (assumption) draws a small tip at the
    // bottom of the screen: a backing rect, the blip's label, and a sprite slot.
    (void)sprite;
    (void)palette;
    const float pad = 8.f;
    const float h = 28.f;
    const float scale = 1.f;
    const float y = (float)screenH - h - pad;
    const float w = (float)screenW * 0.5f;
    const float x = ((float)screenW - w) * 0.5f;
    Hud_Rect(x, y, w, h, 0x000000B0u);                 // translucent backing
    Hud_Rect(x + pad, y + 6.f, 16.f, 16.f, 0xFFFFFF40u); // sprite slot placeholder
    Hud_Text16(x + pad + 22.f, y + 7.f, scale, 0xFFFFFFFFu, label);
}

} // namespace maplegend
