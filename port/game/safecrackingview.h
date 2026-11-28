// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Gui::cSafeCrackingApp, the rendering + input half: it owns a headless safecracking::SafeCracking (the puzzle
// logic, safecracking.cpp) and draws it with the game's OWN sprites, loaded at run time from the user's data.
//
// The safe art is the sprite sheet "SS_SAFECRACKING.bin" (the cSpriteDataSet name string in cSafeCrackingApp::Init,
// 0x81c718). It is loaded the same way the shipped minigames load theirs (SinkingCar / HotWiring): the sprite
// definitions come from the WAD through GameFs, and the sheet texture from the game.pak sprite-slot table
// (gGameDir[23]) via Assets_Texture -- no art is embedded or fabricated here. Sprite SET indices, observed from
// cSafeCrackingApp::Init / ShowNumbers (0x81e708):
//   - the rotating dial face  : set index 21 (the one cSprite::SetSpriteRotation drives, +0x280), pivot app (818,332)
//   - the digital notch readout: set index GetDigitalNumberSpriteId(notch) = notch + 0xb, at app (500, 564)
//   - the stethoscope bars     : set indices 2..9 (eight bars); their per-bar screen position is a run-time .bss
//     table in the binary (0xdd0660, zero-filled at rest), not statically recoverable, so the bars are stacked here
//     (X=0x1f4 confirmed from Init's loop; the per-bar Y = assumption, art real)
//   - the combination digits   : three digit sprites (app fields +0x288/+0x290/+0x298, Init 0x81cd70/+0xb0) at
//     app (0x1f4, 0x233) drawn from the digit set (digit n -> set n+0xb), tinted via cSprite+0x74 with the two
//     packed-RGBA colours 0xFF0000FF (red) / 0xFF00FF00 (green) -> drawn here red while searching, green at target.
// The full-screen safe BODY is a cContainerWnd::AddBgWindow background: Init (0x81c8d0) passes bg-table id 0x6e in
// the same AddBgWindow slot the drug van passes 0xe1/0xe2, so it is gGameDir[26][0x6e] (RenderBg 0x81eef4 draws it
// over the whole (0,0)->(1024,768)). It is loaded by resource id exactly like DrugVanSlashView's van backgrounds.
// Everything that is not safe art (the labels, the tumbler counter) is plain Hud chrome.
//
// The dev-menu start, the per-frame SafeCracking::process() call and the mouse feed live in game.cpp / devmenu.cpp
// (off-limits); this view only supplies load(), render() and the onMouse* handlers for the orchestrator to wire.
#pragma once
#include "safecracking.h"
#include "gfx/spriteset.h"
#include <string>

class SafeCrackingView {
public:
    // Load SS_SAFECRACKING.bin + its sheet (as SinkingCar::start does). false if the sheet cannot be loaded; the
    // caller then reports the missing asset instead of drawing fake art. dataDir is the port's data directory.
    bool load(const std::string& dataDir);
    bool loaded() const { return loaded_; }

    // The puzzle this view drives. The orchestrator starts it (game().start(...) or findWantedRots) from the dev
    // menu and steps it (game().process()) once per 30 Hz frame; this view never advances the logic.
    safecracking::SafeCracking& game() { return game_; }
    const safecracking::SafeCracking& game() const { return game_; }

    // cSafeCrackingApp::Render: draw the background chrome, the dial rotated by SafeCracking::rotation(), the stetho
    // meter from stethoLevel() and the live notch digit, all with the real loaded sprites. W,H are window pixels;
    // they are also cached so the onMouse* handlers can map a cursor to the app's 1024x768 space.
    void render(int W, int H);

    // The dial drag (what cInteractiveSprite + ProcessAnalogRotation 0x81e594 do from a touch): grab, turn, release.
    // Coordinates are window pixels, as the OS delivers the mouse. onMouseUp fires SafeCracking::onRelease().
    void onMouseDown(int x, int y);
    void onMouseMove(int x, int y);
    void onMouseUp(int x, int y);
    bool dragging() const { return dragging_; }

private:
    void toApp(int x, int y, float& ax, float& ay) const;   // window pixel -> app 1024x768 (ortho mode 2)
    float appLeft() const;                                   // cPdaApp::OrthoCorrectionBeforeRender, mode 2
    // cSprite::CalculateVerts: a set sprite about its own offX/offY handle, turned by a 16-bit rotation, in app space.
    // rgba is 0xAARRGGBB (the Hud convention); cSafeCrackingApp tints the combination digits via [cSprite+0x74].
    void drawSprite(int setIndex, float ax, float ay, uint16_t rotation, uint32_t rgba) const;
    // cContainerWnd::AddBgWindow's full-screen safe-body background (cBg::Render), like DrugVanSlashView::drawBackground.
    void drawBackground(unsigned texture) const;

    safecracking::SafeCracking game_;
    SpriteSet sprites_;
    unsigned sheet_ = 0;
    int sheetW_ = 1, sheetH_ = 1;
    unsigned bg_ = 0;              // cSafeCrackingApp::Init (0x81c8d0) AddBgWindow id 0x6e -> gGameDir[26] texture resid
    bool bgOk_ = false;           // the real safe-body background texture resolved and loaded
    bool loaded_ = false;
    int W_ = 0, H_ = 0;             // the last render viewport, for the mouse mapping
    bool dragging_ = false;
    float dragAngle_ = 0;           // the cursor's angle about the dial pivot at the previous mouse event (radians)
};
