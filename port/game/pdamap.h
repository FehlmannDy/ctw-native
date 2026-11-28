// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Gui::cMapApp hosted as a PDA app. cMapApp is the full-screen city map the PDA opens on ePdaApp 0x4a; the heavy
// map rendering and the faithful coordinate core already live in MapScreen (mapscreen.h). This file ports only the
// cMapApp *app behaviour* around that: the PDA app lifecycle (Init / ProcessIntro / Process / ProcessOutro) and the
// touch gestures (drag-to-pan with fling inertia) that drive the view. It is a thin adapter on top of MapScreen.
//
// Ported routines and their evidence (observation unless marked assumption):
//   cMapApp::Init          0x78f014  Base cPdaApp::Init, then cRadar::StartupFromPDA(this, mode 1, 1) +
//                                     cRadar::Rotate(0): the map is hooked into cRadar in full-screen, north-up,
//                                     fixed-scale mode. The port's equivalent is MapScreen::build + open (reuse).
//   cMapApp::ProcessIntro  0x78f190  The appear state machine on +0x13c: wait for the map resources, InitMap, then
//                                     fade cRadar's alpha in by 0.1/frame (0x3dcccccd) until >=1.0, at which point
//                                     AppState (+0x138) becomes 2 (Running). Reproduced as a ~10-frame intro fade.
//   cMapApp::Process       0x790898  Per frame: step cScrollMomentum (pan inertia) and apply the scrolled offset
//                                     (the gamepad/widget-bar menu paths here are the dealers/favourites menus, out
//                                     of this adapter's scope), then ProcessMap.
//   cMapApp::ProcessMap    0x791794  The touch model: cTouchScreen DragStarted stops momentum and arms the drag;
//                                     IsBeingDragged pans cRadar's offset by (prev - current) touch pixels;
//                                     DragEnded seeds cScrollMomentum via ApplyForce(last delta). A SingleClicked
//                                     that barely moved (<0x3f px) sets a GPS waypoint (cMapApp::SetupWaypoint) --
//                                     left unmapped here (needs cGPS; see notes).
//   cMapApp::ProcessOutro  0x790868  Calls the base shutdown (vtable +0x10) and sets AppState (+0x138) to its gone
//                                     state. Reproduced as an instant onLeave.
//   cScrollMomentum        0x7be73c..0x7be814  Stop / ApplyForce / Process: the pan fling. Velocity decays by 0.75
//                                     once per period while its magnitude is >= 1.0 px (Q12 4096), and snaps to 0
//                                     below that. Ported faithfully in MapPanMomentum below.
//
// Not ported (documented, not faked): the dealers/favourites/contacts/POI sub-menus and their gamepad navigation
// (cWidgetBar), the GPS waypoint plot on tap (cMapApp::SetupWaypoint / cGPS), the taxi overlay (cMapTaxiOverlay),
// the PDA tutorials, and the per-zone name label (cMapApp::ProcessMap's GXT lookup). There is NO pinch/zoom gesture
// in cMapApp -- the bottom-screen map is a single fixed scale (2.5 world units/pixel); zoom() below is a port-only
// convenience (e.g. for a mouse wheel) and is marked as such.
#pragma once
#include "pda.h"
#include "radar.h"
#include "mapscreen.h"
#include <cmath>
#include <cstdint>

class Game;

// cScrollMomentum (Gui::cScrollMomentum, 0x7be73c..). The kinetic-scroll fling behind the map's drag. Velocity is
// kept in cFixed<20,12> pixels (Q12). Verified against ApplyForce (0x7be744), Process (0x7be7a0) and Stop (0x7be73c).
class MapPanMomentum {
public:
    // cScrollMomentum::Stop (0x7be73c): zero the velocity (the direction vector is left as-is, as in the binary).
    void stop() { vx_ = vy_ = 0; active_ = false; }

    // cScrollMomentum::ApplyForce (0x7be744): the force becomes the velocity. (The binary also stores a normalised
    // direction at +0x00 for other consumers; the scroll-offset path only reads the velocity, so the port omits it.)
    // force components are Q12 pixels.
    void applyForce(int32_t fxQ12, int32_t fyQ12) {
        vx_ = fxQ12;
        vy_ = fyQ12;
        active_ = (vx_ | vy_) != 0;
    }

    // cScrollMomentum::Process (0x7be7a0): once the speed drops below 1.0 px (magnitude < 4096 Q12) stop; otherwise
    // decay each component by 0.75 (the binary's (v*3)>>2). The binary gates the decay to every `period` frames via a
    // global frame counter; the port uses period 1 (decay every frame) -- see the note. Returns true while moving.
    bool process() {
        const int64_t mag2 = (int64_t)vx_ * vx_ + (int64_t)vy_ * vy_;
        const uint32_t mag = (uint32_t)std::sqrt((double)mag2);   // ucvtf / fsqrt / fcvtzu: floor(sqrt)
        if (mag < 4096u) { vx_ = vy_ = 0; active_ = false; return false; }
        vx_ = (int32_t)(((int64_t)vx_ * 3) >> 2);                 // (v*3)>>2 == v*0.75
        vy_ = (int32_t)(((int64_t)vy_ * 3) >> 2);
        active_ = true;
        return true;
    }

    bool active() const { return active_; }
    int32_t vxQ12() const { return vx_; }   // cScrollMomentum +0x08
    int32_t vyQ12() const { return vy_; }   // cScrollMomentum +0x0c

private:
    int32_t vx_ = 0, vy_ = 0;
    bool active_ = false;
};

// Gui::cMapApp hosted inside the PDA shell. Register it in the PDA app factory for ePdaApp kAppMap (0x4a) and hand
// it the RadarMap (built once in onLoad) and the Game (for the player-centre and the MapScreen render).
class MapPdaApp : public pda::PdaApp {
public:
    // The orchestrator supplies the radar overview tiles (RadarMap) and the Game. The RadarMap is copied into the
    // owned MapScreen in onLoad (MapScreen::build). appId defaults to kAppMap (0x4a).
    MapPdaApp(const RadarMap& map, const Game& game, int appId = pda::kAppMap);
    // Or default-construct and wire with setMap/setGame before the app loads.
    explicit MapPdaApp(int appId = pda::kAppMap) : pda::PdaApp(appId) {}

    // Optional setters if the factory would rather default-construct and wire afterwards.
    void setMap(const RadarMap& map) { map_ = map; }
    void setGame(const Game& game) { game_ = &game; }

    // Access to the adapted MapScreen (for Lane B's blip/legend hooks, or a port-only zoom binding).
    MapScreen& screen() { return screen_; }
    const MapScreen& screen() const { return screen_; }

    // Port-only convenience (NOT a cMapApp gesture -- the real map is fixed-scale). For a mouse-wheel/keyboard zoom
    // the orchestrator may wire this; it just forwards to MapScreen::zoomBy.
    void zoom(float factor) { screen_.zoomBy(factor); }

    // ---- cPdaApp hooks (see pda.h) ----
    bool onLoad() override;                       // cMapApp::Init: build + open the map on the player
    bool onAppear() override;                     // cMapApp::ProcessIntro: the alpha fade-in
    void onProcess() override;                    // cMapApp::Process: step + apply the pan momentum
    void onInput(const pda::Touch& t) override;   // cMapApp::ProcessMap: drag-to-pan + fling
    bool onLeave() override;                       // cMapApp::ProcessOutro: instant
    void onRender(int w, int h) const override;    // MapScreen::render into the content area

private:
    RadarMap map_;
    MapScreen screen_;
    const Game* game_ = nullptr;

    // ProcessIntro alpha fade (+0x874 in the binary): 0 -> 1 at 0.1/frame. Behaviourally it delays "Running" ~10
    // frames, as the real app does while the radar fades in.
    float introAlpha_ = 0.f;

    // ProcessMap drag state.
    MapPanMomentum momentum_;    // cMapApp +0x190 (cScrollMomentum)
    bool dragging_ = false;      // cMapApp +0x1b4
    bool prevDown_ = false;
    float prevX_ = 0.f, prevY_ = 0.f;
    int32_t lastDxQ12_ = 0, lastDyQ12_ = 0;   // cMapApp +0x1ac/+0x1b0: last frame's drag delta, Q12

    friend struct MapPdaTestAccess;
};
