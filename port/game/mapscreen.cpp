// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
#include "mapscreen.h"
#include "game.h"
#include "hud.h"
#include "gfx/assets.h"
#include <glad/gl.h>
#include <algorithm>
#include <cmath>

// ===========================================================================
// cMapApp coordinate core (verified against Gui::cMapApp, libGame.so)
// ===========================================================================

// cMapApp::BottomScreenToWorldCoordinates (0x793768). Replicated bit-for-bit:
// the X axis is an smaddl (signed 32x32 + 64) then a logical >>12; the Y axis is
// a 64-bit madd on the sign-extended coordinate then a logical >>12. The Y
// multiplier 0x0FFFFFFFD800 is (2^44 - 10240); for the stored 32-bit result it is
// exactly equivalent to a scale of -10240, so the map is +2.5 world units/pixel in
// X and -2.5 in Y about the corner at world (-3540, +2520).
void MapScreen::bottomScreenToWorld(int32_t sxQ12, int32_t syQ12,
                                    int32_t originX, int32_t originY, int32_t outWorld[3]) {
    const int32_t w8 = (int32_t)((uint32_t)sxQ12 + (uint32_t)originX * 4096u);   // add w8,w10,w8 lsl#12
    const uint64_t tx = (uint64_t)((int64_t)w8 * 10240 + 0x0FF22C000000LL);       // smaddl + bias
    outWorld[0] = (int32_t)(uint32_t)(tx >> 12);                                  // lsr #0xc, store w

    const int32_t w9 = (int32_t)((uint32_t)syQ12 + (uint32_t)originY * 4096u);    // add w9,w9,w10 lsl#12
    const uint64_t x9 = (uint64_t)(int64_t)w9;                                    // sxtw
    const uint64_t ty = x9 * 0x00000FFFFFFFD800ULL + 0x00000009D8000000ULL;       // madd (scale -10240)
    outWorld[1] = (int32_t)(uint32_t)(ty >> 12);                                  // lsr #0xc, store w

    outWorld[2] = 0;                                                              // str wzr, [x2,#8]
}

void MapScreen::worldToBottomScreen(int32_t wxQ12, int32_t wyQ12,
                                    int32_t originX, int32_t originY, int32_t outScreen[2]) {
    // Inverse of the affine map above (world Q12 -> bottom-screen Q12). After the
    // forward routine's >>12 and 32-bit truncation the effective world corner is
    // -3540*4096 in X (= -0xDD4000000 in the *4096 domain) and +2520*4096 in Y
    // (= +0x9D8000000); these, not the forward routine's raw 44-bit biases, invert it.
    const int64_t sx = ((int64_t)wxQ12 * 4096 + 0xDD4000000LL) / 10240 - (int64_t)originX * 4096;
    const int64_t sy = (0x9D8000000LL - (int64_t)wyQ12 * 4096) / 10240 - (int64_t)originY * 4096;
    outScreen[0] = (int32_t)sx;
    outScreen[1] = (int32_t)sy;
}

// ===========================================================================
// Full-screen map rendering
//
// NOTE ON MAP ART: the original cMapApp lays the bottom-screen big map out through
// the DS background tile layers (cBgManager; InitMap @0x78f3f8-0x78f414 resets the
// bg memory manager and restores the default bg palette) and cTopScreenMap for the
// top screen -- a higher-resolution art set than cRadar's overview textures. That
// art is not available to the port, so this approximates the full-screen map with
// the port's RadarMap overview tiles (the same resource 2760 the radar uses, which
// shares the world origin verified above). The coordinate core and the world origin
// are faithful; the tile imagery is an approximation, clearly an observation here.
// ===========================================================================

namespace {
// Inverse of RadarMap::worldToMap (the radar overview scale 0x666 / 2^24): one
// overview map-pixel spans 2^24 / 0x666 world units about the corner (-3540, +2520).
constexpr double kWorldPerMapPixel = 16777216.0 / 1638.0;   // 2^24 / 0x666
void mapToWorld(double mx, double my, int32_t out[3]) {
    out[0] = (int32_t)std::lround(mx * kWorldPerMapPixel - 3540.0 * 4096.0);
    out[1] = (int32_t)std::lround(2520.0 * 4096.0 - my * kWorldPerMapPixel);
    out[2] = 0;
}
void fillDisc(float x, float y, float r, float rr, float gg, float bb, float aa) {
    glColor4f(rr, gg, bb, aa);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(x, y);
    for (int i = 0; i <= 32; ++i) {
        const float a = i * (2.f * 3.14159265358979f / 32.f);
        glVertex2f(x + std::sin(a) * r, y + std::cos(a) * r);
    }
    glEnd();
}
}  // namespace

bool MapScreen::build(const RadarMap& map) {
    map_ = map;
    return built();
}

void MapScreen::open(const Game& game) {
    open_ = true;
    recentreOnPlayer(game);
}

void MapScreen::recentreOnPlayer(const Game& game) {
    game.focus(centre_);
    clampView();
}

void MapScreen::clampView() {
    zoom_ = std::clamp(zoom_, 0.5f, 8.0f);
    // Keep the centre within the tiled map extent. Map-pixel space uses the radar
    // overview scale (mapToWorld), about the same corner the core shares: (-3540, +2520).
    const float mapW = map_.columns() * 256.f, mapH = map_.rows() * 256.f;
    if (mapW <= 0.f || mapH <= 0.f) return;
    const float minMapX = map_.originX(), maxMapX = map_.originX() + mapW;
    const float minMapY = map_.originY(), maxMapY = map_.originY() + mapH;
    float cmx, cmy;
    RadarMap::worldToMap(centre_, cmx, cmy);
    const float clx = std::clamp(cmx, minMapX, maxMapX);
    const float cly = std::clamp(cmy, minMapY, maxMapY);
    if (clx != cmx || cly != cmy) mapToWorld(clx, cly, centre_);
}

void MapScreen::pan(float dxPixels, float dyPixels) {
    // A screen-pixel drag moves the centre by the opposite amount in map-pixels.
    float cmx, cmy;
    RadarMap::worldToMap(centre_, cmx, cmy);
    mapToWorld(cmx - dxPixels / zoom_, cmy - dyPixels / zoom_, centre_);
    clampView();
}

void MapScreen::zoomBy(float factor) {
    zoom_ *= factor;
    clampView();
}

bool MapScreen::worldToScreen(const int32_t pos[3], float& sx, float& sy) const {
    if (viewW_ <= 0 || viewH_ <= 0) return false;
    float mx, my;
    RadarMap::worldToMap(pos, mx, my);
    sx = viewW_ * 0.5f + (mx - centreMapX_) * zoom_;
    sy = viewH_ * 0.5f + (my - centreMapY_) * zoom_;
    return true;
}

void MapScreen::render(const Game& game, int width, int height) const {
    if (!built() || width <= 0 || height <= 0) return;
    viewW_ = width;
    viewH_ = height;
    RadarMap::worldToMap(centre_, centreMapX_, centreMapY_);
    const float cmx = centreMapX_, cmy = centreMapY_;
    const float halfW = width * 0.5f, halfH = height * 0.5f;

    Hud_Begin(width, height);
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_TEXTURE_BIT);
    glDisable(GL_ALPHA_TEST);

    // Dark backdrop behind the map.
    Hud_Rect(0.f, 0.f, (float)width, (float)height, 0x0A1A22FFu);

    // Visible map-pixel window, converted to tile columns/rows (256-pixel tiles).
    const float left = cmx - halfW / zoom_, right = cmx + halfW / zoom_;
    const float top = cmy - halfH / zoom_, bottom = cmy + halfH / zoom_;
    const int firstCol = (int)std::floor((left - map_.originX()) / 256.f);
    const int lastCol = (int)std::floor((right - map_.originX()) / 256.f);
    const int firstRow = (int)std::floor((top - map_.originY()) / 256.f);
    const int lastRow = (int)std::floor((bottom - map_.originY()) / 256.f);
    const float tileSize = 256.f * zoom_;

    glEnable(GL_TEXTURE_2D);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    for (int row = firstRow; row <= lastRow; ++row) {
        for (int col = firstCol; col <= lastCol; ++col) {
            int id = map_.texture(col, row);
            if (id < 0) id = map_.defaultTexture();
            const unsigned tex = id < 0 ? 0u : Assets_Texture(id);
            if (!tex) continue;
            const float mapX0 = map_.originX() + col * 256.f;
            const float mapY0 = map_.originY() + row * 256.f;
            const float x0 = halfW + (mapX0 - cmx) * zoom_;
            const float y0 = halfH + (mapY0 - cmy) * zoom_;
            glBindTexture(GL_TEXTURE_2D, tex);
            glBegin(GL_QUADS);
            glTexCoord2f(0.f, 0.f); glVertex2f(x0, y0);
            glTexCoord2f(1.f, 0.f); glVertex2f(x0 + tileSize, y0);
            glTexCoord2f(1.f, 1.f); glVertex2f(x0 + tileSize, y0 + tileSize);
            glTexCoord2f(0.f, 1.f); glVertex2f(x0, y0 + tileSize);
            glEnd();
        }
    }
    glDisable(GL_TEXTURE_2D);

    // Lane B draws the mission/dealer blips over the tiles.
    drawBlips(game, width, height);

    // Player marker at the player's (or his car's) world position.
    int32_t focus[3];
    game.focus(focus);
    float px, py;
    if (worldToScreen(focus, px, py)) {
        fillDisc(px, py, 7.f, 0.f, 0.f, 0.f, 0.6f);          // outline
        fillDisc(px, py, 5.f, 0.4f, 0.75f, 1.f, 1.f);        // marker
    }

    // Lane B draws the legend last, on top of everything.
    drawLegend(game, width, height);

    glPopAttrib();
    Hud_End();
}

// Documented stubs: the orchestrator wires Lane B's maplegend.* functions through
// these hooks. Until then they are no-ops -- this lane never draws blips or legend.
void MapScreen::drawBlips(const Game& game, int width, int height) const {
    if (drawBlipsHook_) drawBlipsHook_(*this, game, width, height);
}

void MapScreen::drawLegend(const Game& game, int width, int height) const {
    if (drawLegendHook_) drawLegendHook_(*this, game, width, height);
}
