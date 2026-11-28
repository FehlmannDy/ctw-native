// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Gui::cMapApp hosted inside the PDA shell. See pdamap.h for the per-routine evidence. This is a thin adapter: the
// map rendering and the cMapApp coordinate core live in MapScreen; here we reproduce the app lifecycle and the
// drag-to-pan + fling touch behaviour on top of it.
#include "pdamap.h"
#include "game.h"
#include <cmath>

MapPdaApp::MapPdaApp(const RadarMap& map, const Game& game, int appId)
    : pda::PdaApp(appId), map_(map), game_(&game) {}

// cMapApp::Init (0x78f014) + InitMap (0x78f2c4): the real routine hooks the map into cRadar
// (StartupFromPDA, full-screen mode 1, Rotate(0) -> north-up, fixed scale) once its resources are ready. The port's
// equivalent is to build the MapScreen from the radar overview tiles and centre it on the player. onLoad returns
// true once the tiles are adopted (the "resources ready" gate); if no RadarMap was supplied it keeps waiting.
bool MapPdaApp::onLoad() {
    if (!screen_.built()) {
        if (map_.columns() <= 0) return false;   // resources not ready yet (cResourceManager::IsStreaming)
        if (!screen_.build(map_)) return false;
    }
    if (game_) screen_.open(*game_);              // Init's recentre-on-player (cRadar offset from the player pos)
    return true;
}

// cMapApp::ProcessIntro (0x78f190): after InitMap the app fades cRadar's alpha from 0 to 1 by 0.1 each frame
// (constant 0x3dcccccd) and only then sets its state to Running (+0x138 = 2). We reproduce the ~10-frame fade so the
// app settles into Running on the same cadence; MapScreen draws at full alpha, so the value is timing only.
bool MapPdaApp::onAppear() {
    introAlpha_ += 0.1f;                          // 0x3dcccccd
    if (introAlpha_ < 1.0f) return false;
    introAlpha_ = 1.0f;
    return true;
}

// cMapApp::Process (0x790898) -> the cScrollMomentum step and the scrolled-offset apply, then ProcessMap. While a
// drag is active the velocity is zero (Stop was called), so the momentum step is a no-op and the drag itself pans in
// onInput; after release the seeded velocity flings the map here and decays to rest.
void MapPdaApp::onProcess() {
    if (dragging_) return;
    if (momentum_.process()) {
        // cMapApp::ProcessMap: originX/Y += velocity >> 12 (Q12 -> whole pixels) each frame. MapScreen::pan moves the
        // centre the opposite way, i.e. the content follows the fling, matching the binary's offset add.
        screen_.pan((float)(momentum_.vxQ12() >> 12), (float)(momentum_.vyQ12() >> 12));
    }
}

// cMapApp::ProcessMap (0x791794) touch handling. The binary reads gTouchScreen (DragStarted / IsBeingDragged /
// DragEnded). The PDA shell hands us a per-frame Touch already clipped to the content area, so we derive the same
// three edges from it: press = DragStarted, held = IsBeingDragged, release = DragEnded.
//   drag:    pan by (current - previous) touch pixels  (binary adds (prev - current) to the scroll origin, i.e. the
//            same content-follows-finger motion; MapScreen::pan takes the content delta).
//   release: ApplyForce(last frame's delta) -> the fling, decayed in onProcess (cScrollMomentum).
// A near-stationary single click would set a GPS waypoint in the binary (cMapApp::SetupWaypoint); that path needs
// cGPS and is intentionally not reproduced here.
void MapPdaApp::onInput(const pda::Touch& t) {
    const bool down = t.down;
    const bool press = down && !prevDown_;        // cTouchScreen::DragStarted edge

    if (press) {
        momentum_.stop();                         // DragStarted stops any running fling
        dragging_ = true;
        prevX_ = t.x;
        prevY_ = t.y;
        lastDxQ12_ = lastDyQ12_ = 0;
    } else if (down && dragging_) {               // cTouchScreen::IsBeingDragged
        const float dx = t.x - prevX_;
        const float dy = t.y - prevY_;
        screen_.pan(dx, dy);
        lastDxQ12_ = (int32_t)std::lround(dx) << 12;   // stored at +0x1ac/+0x1b0 (Q12) for ApplyForce
        lastDyQ12_ = (int32_t)std::lround(dy) << 12;
        prevX_ = t.x;
        prevY_ = t.y;
    } else if (!down && dragging_) {              // cTouchScreen::DragEnded
        dragging_ = false;
        momentum_.applyForce(lastDxQ12_, lastDyQ12_);
    }

    prevDown_ = down;
}

// cMapApp::ProcessOutro (0x790868): the base shutdown then the app's gone state. Nothing map-specific survives the
// close in the port, so the outro completes immediately.
bool MapPdaApp::onLeave() {
    return true;
}

// Draw the map into the PDA content area. cMapApp::Render draws the radar/blips through cRadar; the port reuses
// MapScreen::render, which sets up its own full-screen ortho and runs Lane B's blip/legend hooks.
void MapPdaApp::onRender(int w, int h) const {
    if (game_ && screen_.built()) screen_.render(*game_, w, h);
}
