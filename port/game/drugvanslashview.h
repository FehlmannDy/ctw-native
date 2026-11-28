// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Gui::cDrugVanSlashApp view: the rendering + mouse input for the drug-van slashing minigame. It owns the headless
// swipe/state logic (drugvanslash::DrugVanSlash) and drives it from real loaded assets each frame, mirroring how
// SinkingCar / HotWiring host their minigames (sinkingcar.cpp / hotwiring.cpp).
//
// The app runs in a plain 1024 x 768 ortho space (y down), the waypoints in drugvanslash.h are in that space
// (observation: the const polyline table at 0x485118 holds those pixel coordinates; cDrugVanSlashApp::Init
// 0x7f3f7c calls cContainerWnd::AddBgWindow with w=0x400 h=0x300). The window maps a window pixel (mx,my) to the
// app's (mx*1024/W, my*768/H), the same plain stretch cSinkingCarApp uses (assumption: the van app shares sinking
// car's plain 1024x768 ortho, not hotwiring's right-aligned mode).
//
// Assets (confirmed from the user's own game.pak, see drugvanslashview.cpp):
//   * The van backgrounds are gGameDir[26] theme-table entries 0xe1 and 0xe2 (AddBgWindow in Init), loaded as
//     textures from game.pak by resource id exactly as cSinkingCar loads its interior. These ARE the distinctive
//     van art and are real loaded sprites, never fabricated.
//   * The slash / tear sprites come from the loose sheet SS_DrugVanSlashing.bin (the cSpriteDataSet name string at
//     0x46e1e4) plus its sheet texture via the gGameDir[23] slot table, the same pipeline SS_SinkingCar.bin uses.
//     If that .bin is not present in the data dir, the slash cuts are drawn as plain geometry (lines) and the
//     missing asset is reported; no van / tear art is invented.
#pragma once

#include "drugvanslash.h"
#include "gfx/spriteset.h"

#include <cstdint>
#include <functional>
#include <string>

class DrugVanSlashView {
public:
    // Load the van backgrounds (game.pak) and, if present, the SS_DrugVanSlashing sprite sheet. Returns true if at
    // least the van backgrounds loaded (the minigame is playable: real van art + drawn cuts). Call once before start.
    bool load(const std::string& dataDir);

    // cDrugVanSlashApp::Reset: arm a fresh attempt and hand the logic its sfx/award hooks.
    void start(drugvanslash::VanKind kind = drugvanslash::VanKind::Drugs, int itemId = 0, int qty = 0,
               std::function<void(bool success)> done = {});
    void stop();
    bool active() const { return game_.active(); }

    // cDrugVanSlashApp::Process for the non-slashing states (tear / result): one 30 Hz frame. Swipes drive Slashing.
    void process();

    void render(int W, int H) const;

    // The mouse as cTouchScreen's touch, in window pixels. A press begins a swipe; moves extend it and cut each
    // segment it covers; a release ends it, and a release that barely moved while Grabbing grabs the stash.
    void onMouseDown(int x, int y, int W, int H);
    void onMouseMove(int x, int y, int W, int H);
    void onMouseUp(int x, int y, int W, int H);

    bool spriteSheetLoaded() const { return sheetOk_; }
    bool backgroundsLoaded() const { return bgOk_; }
    // The data-dir-relative name of an asset that could not be loaded, or nullptr if everything loaded.
    const char* missingAsset() const { return sheetOk_ ? nullptr : "SS_DrugVanSlashing.bin"; }

    drugvanslash::DrugVanSlash& game() { return game_; }
    const drugvanslash::DrugVanSlash& game() const { return game_; }

    // Forwarded to the logic's playSfx / awardStash hooks by start(); set before start().
    std::function<void(int sample, int volume)> playSfx;
    std::function<void(drugvanslash::VanKind kind, int itemId, int qty)> awardStash;

private:
    void toApp(int x, int y, int W, int H, float& ax, float& ay) const;
    // Offer the current stroke (dragFrom_ -> p) to the logic, chaining through every door edge it covers.
    void feedSwipe(const drugvanslash::Vec2& p);
    void drawBackground(int texture, int W, int H) const;
    void drawSheetSprite(int index, float x, float y, float scale, float alpha, int W, int H) const;
    void drawLine(const drugvanslash::Vec2& a, const drugvanslash::Vec2& b, float width, uint32_t rgba,
                  int W, int H) const;

    drugvanslash::DrugVanSlash game_;

    // assets
    SpriteSet sprites_;
    unsigned sheet_ = 0;
    int sheetW_ = 1, sheetH_ = 1;
    bool sheetOk_ = false;
    int vanBg_ = 0, vanOverlay_ = 0;   // texture ids from backgrounds[0xe1] / [0xe2]
    bool bgOk_ = false;
    std::string dataDir_;

    // swipe accumulation (window->app space)
    bool dragging_ = false;
    drugvanslash::Vec2 dragFrom_{};   // the start of the un-cut part of the current swipe
    drugvanslash::Vec2 dragLast_{};   // the latest finger position
    float dragTravel_ = 0.f;          // total path length of the current press (to tell a tap from a swipe)
};
