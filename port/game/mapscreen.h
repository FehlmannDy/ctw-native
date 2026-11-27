// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// cMapApp core: the full-screen city map with zoom/pan and a player marker.
//
// The coordinate maths (MapScreen::bottomScreenToWorld and its inverse) are ported
// exactly from Gui::cMapApp (libGame.so). The full-screen *rendering* reuses the
// port's RadarMap overview tiles (radar.h) as an approximation of the map art, which
// is documented at the render() definition. Blip and legend drawing are deliberately
// left as hooks for Lane B (maplegend.*); this class never draws them itself.
#pragma once
#include "radar.h"
#include <cstdint>
#include <functional>

class Game;

class MapScreen {
public:
    MapScreen() = default;

    // ---- faithful cMapApp coordinate core -----------------------------------
    //
    // cMapApp::BottomScreenToWorldCoordinates (0x793768). Maps a bottom-screen
    // point (cFixed<20,12>, i.e. Q12 pixels) to a world position (Q12). The two
    // origin values are cRadar's map scroll offset (the cRadar singleton +0x80 and
    // +0x84), in whole pixels; they carry the map's pan. The constants below are
    // read straight from the routine:
    //   world.x = ((sx + originX*4096) *  10240 + 0x0FF22C000000) >> 12
    //   world.y = ((sy + originY*4096) * -10240 + 0x009D8000000) >> 12
    //   world.z = 0
    // 10240 (Q12) = 2.5 world units per bottom-screen pixel; the Q12 biases are the
    // map's corner, world (-3540, +2520) -- the same origin radar.cpp uses.
    // Observation: verified against the disassembly at 0x793768.
    static void bottomScreenToWorld(int32_t sxQ12, int32_t syQ12,
                                    int32_t originX, int32_t originY, int32_t outWorld[3]);

    // Algebraic inverse of the above (world Q12 -> bottom-screen Q12 pixels).
    // Assumption: derived by inverting the affine map; not a separate routine in the
    // binary. Exact up to the forward routine's final >>12 truncation.
    static void worldToBottomScreen(int32_t wxQ12, int32_t wyQ12,
                                    int32_t originX, int32_t originY, int32_t outScreen[2]);

    // ---- full-screen map app ------------------------------------------------
    bool build(const RadarMap& map);            // adopt the radar overview tiles + origin
    bool built() const { return map_.columns() > 0; }

    void open(const Game& game);                // centre on the player, mark open
    void close() { open_ = false; }
    bool isOpen() const { return open_; }

    // Input: pan by a screen-pixel delta, zoom by a multiplicative factor. Both clamp.
    void pan(float dxPixels, float dyPixels);
    void zoomBy(float factor);
    void recentreOnPlayer(const Game& game);

    // Draw the map full-screen (pixels, origin top-left). Sets up its own ortho.
    void render(const Game& game, int width, int height) const;

    // Render-space projection for Lane B's blips/legend: world position -> screen
    // pixel, using the same zoom/pan as the last render(). Valid while a hook runs.
    bool worldToScreen(const int32_t pos[3], float& sx, float& sy) const;
    float zoom() const { return zoom_; }

    // ---- Lane B hooks (maplegend.*) -----------------------------------------
    // The orchestrator wires these to Lane B's legend/blip functions. They are
    // called once per render (blips over the tiles+marker, legend last) with this
    // MapScreen (for worldToScreen) and the Game. Default: no-op.
    using DrawHook = std::function<void(const MapScreen&, const Game&, int width, int height)>;
    void setBlipsHook(DrawHook h) { drawBlipsHook_ = std::move(h); }
    void setLegendHook(DrawHook h) { drawLegendHook_ = std::move(h); }

private:
    void drawBlips(const Game& game, int width, int height) const;   // documented stub -> hook
    void drawLegend(const Game& game, int width, int height) const;  // documented stub -> hook
    void clampView();

    RadarMap map_;
    int32_t centre_[3] = {0, 0, 0};   // world position shown at the screen centre
    float zoom_ = 2.0f;               // screen pixels per RadarMap map-pixel
    bool open_ = false;

    // Stashed during render() so worldToScreen (and the hooks) can project.
    mutable int viewW_ = 0, viewH_ = 0;
    mutable float centreMapX_ = 0.f, centreMapY_ = 0.f;

    DrawHook drawBlipsHook_;
    DrawHook drawLegendHook_;
};
