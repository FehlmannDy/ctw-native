// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Faithful cMapApp helpers for the big map (Gui::cMapApp, libGame.so):
//   - GetDealerSpriteAndPalette (0x790038): dealer blip sprite/palette lookup
//   - GetDistanceString         (0x798cf8): world-distance -> display number/string
//   - DisplayMapLegend          (0x799728): cursor-over-blip legend hit test
//
// These are standalone (no MapScreen/radar.cpp dependency) so the map screen can
// call them directly. Coordinates are cFixed<20,12>: raw integers with 12 frac
// bits (real value = raw / 4096).
#pragma once
#include <cstdint>
#include <string>

namespace maplegend {

// ---- cMapApp::GetDealerSpriteAndPalette (0x790038) -------------------------
// The save stores one "dealer tip" record per known dealer. The original walks
// SaveGameImpl::GetDealerTip(i) until record byte +0xB equals the queried dealer
// type, then derives two (sprite, palette) pairs from that record. The save
// lookup lives outside this lane, so DealerTip carries the exact record fields
// the routine reads; the orchestrator fills it from the matched record.
struct DealerTip {
    uint16_t drugId = 0;      // record +0x00: the drug id (0x448..0x452) sold here
    uint8_t  flag4  = 0;      // record +0x04: selects the "visited" sprite/palette
    uint8_t  flag7  = 0;      // record +0x07
    uint8_t  flagA  = 0;      // record +0x0A
    bool     hasDealerBase = true;  // original x26: *(g+0x888)+type*0x30+0x498 != 0
};

// Pure drug-id -> map sprite id switch from 0x790140..0x7901bc (observation).
// Unknown ids fall through to the default dot (0x17).
uint16_t mapDealerDrugSprite(uint16_t drugId);

// Reproduces 0x7900fc..0x790204 given the matched record. Writes the primary
// (sprite1,palette1) and the secondary (sprite2,palette2) the map draws for a
// dealer blip. Returns the record's semantics flag (original w24) for callers
// that need it; the found/not-found distinction is handled by the caller via
// the search. palette1 is always 9 (observation).
void dealerSpriteAndPalette(const DealerTip& tip,
                            uint16_t& sprite1, uint32_t& palette1,
                            uint16_t& sprite2, uint32_t& palette2);

// Not-found result (0x7900c4): sprite 0x17 / palette 9 on both pairs, returns false.
// Convenience wrapper matching the orchestrator's requested shape. `found` tells
// the caller whether the dealer search located a record (index >= 0 originally).
bool mapBlipSprite(bool found, const DealerTip& tip,
                   uint16_t& sprite1, uint32_t& palette1,
                   uint16_t& sprite2, uint32_t& palette2);

// ---- cMapApp::GetDistanceString (0x798cf8) ---------------------------------
// Exact integer pipeline from 0x798d1c..0x798d70: squared 3D distance in fixed
// raw units -> sqrt (as double) -> magic scale -> the integer the HUD prints.
// a and b are tv3d<cFixed<20,12>> as {x,y,z} raw ints.
int32_t mapDistanceNumber(const int32_t a[3], const int32_t b[3]);

// The decimal number followed by the distance-unit suffix. The original appends
// PDAText()->GetString(0x5A) (GXT id 90) via UnicodeStrcat; that string is game
// text data, so the suffix is passed in (empty by default). Rendering can go
// through Hud_Text.
std::string mapDistanceString(const int32_t a[3], const int32_t b[3],
                              const std::string& unitSuffix = "");

// ---- cMapApp::DisplayMapLegend (0x799728) ----------------------------------
// A map "blip" as the hit test reads it (stride 0x20 in the original array at
// cRadar+0xAD4): position and the legend string id baked into the record.
struct LegendBlip {
    int32_t x = 0;              // +0x00 (cFixed raw)
    int32_t y = 0;              // +0x04 (cFixed raw)
    int32_t legendStringId = 0; // +0x0C: GXT id shown when the cursor is over it
};

// Hit test from 0x7997d4..0x799840: is the cursor within a +/-5.0 (fixed) box of
// a blip? On a hit, outStringId receives that blip's +0x0C field and returns
// true (the original then shows its radar sprite + that string as a help tip).
// cursor is {x,y,z} raw fixed; only x,y are used.
bool mapLegendHitTest(const int32_t cursor[3], const LegendBlip* blips, int count,
                      int32_t& outStringId);

// The per-type legend GXT id. The original indexes a 117-entry byte jump table
// at 0x483C04 by blip type (0..0x74) into cases that set the GXT id (0x539..0x55B),
// several of which further branch on a sub-type (original w23) read from the map
// sprite. That jump table is extracted game data and is NOT reproduced here; this
// helper only ports the in-code sub-type disambiguation. `type` is the blip type
// byte, `subType` the sprite's +0x1F field. Returns the GXT id, or 0 if the type
// needs the jump table (see maplegend.cpp). resolved is set false in that case.
uint16_t mapLegendStringForType(int type, int subType, bool& resolved);

// Draws one resolved legend entry (radar sprite + its label) with the port's HUD
// primitives. The original hands the sprite+string to cPdaHelpManager so its exact
// panel geometry is not in 0x799728; this layout is the port's (assumption).
void drawMapLegend(int screenW, int screenH, uint16_t sprite, uint8_t palette,
                   const std::u16string& label);

} // namespace maplegend
