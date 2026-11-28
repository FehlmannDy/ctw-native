// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Rendering + mouse input for Gui::cDrugVanSlashApp. The swipe/state machine lives, headless, in drugvanslash.cpp;
// this view draws it from real loaded assets and feeds it the mouse, like sinkingcar.cpp / hotwiring.cpp.
//
// Asset pipeline (confirmed, not assumed):
//   * cDrugVanSlashApp::Init (0x7f3f7c) calls cContainerWnd::AddBgWindow twice with background-table ids 0xe1 and
//     0xe2 over the full 1024x768 window. Those ids index the PDA theme background table (gGameDir u16[26]); in the
//     user's own game.pak gGameDir[26]=0xf6b, and backgrounds[0xe1]->texture resid 0x104d (a 42-page image, the van)
//     and backgrounds[0xe2]->0x104e (3 pages, the overlay), both present. They load from game.pak by resource id the
//     same way cSinkingCar loads its interior (sinkingcar.cpp), i.e. the PDA/HUD sheet pipeline. REAL van art.
//   * Init builds a cSpriteDataSet from "SS_DrugVanSlashing.bin" (the name string at 0x46e1e4) for the slash/tear
//     sprites, whose sheet texture resolves through the gGameDir u16[23] slot table (=0xaf4). Same loose-sheet path
//     as SS_SinkingCar.bin. If that .bin is absent from the data dir, the cuts are drawn as plain lines and the
//     missing asset is reported; nothing is fabricated.
#include "drugvanslashview.h"

#include "hud.h"
#include "gfx/assets.h"
#include "os/gamefs.h"
#include "os/pak.h"

#include <glad/gl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
using drugvanslash::Vec2;

// Background-table indices for the two van backgrounds (cDrugVanSlashApp::Init AddBgWindow args). Observation.
constexpr int kVanBgId = 0xe1;
constexpr int kVanOverlayId = 0xe2;

// The slash blade sprite in SS_DrugVanSlashing for each of the four door segments. Observation: the sheet's
// sprites 0 and 3 are tall (62x83) and 1 and 2 are wide (83x59), each with a centred handle (offX/offY ~= w/2,h/2).
// The door polyline alternates vertical (segments 0,2) and horizontal (segments 1,3), so the two tall sprites take
// the vertical cuts and the two wide ones the horizontal cuts, drawn centred on the cut with no rotation.
// ASSUMPTION: the exact segment->sprite pairing (cSlash::Init reads it from a runtime .bss meta table at 0xdd0390,
// not statically resolvable); the orientation/handle match makes this the natural pairing.
constexpr int kSlashSprite[drugvanslash::kSlashCount] = {0, 1, 3, 2};

uint16_t tableEntry(const std::vector<uint8_t>& t, int i) {
    uint16_t v = 0;
    if ((size_t)(i + 1) * 2 <= t.size()) std::memcpy(&v, t.data() + i * 2, 2);
    return v;
}

float dist(const Vec2& a, const Vec2& b) {
    const float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}
}  // namespace

bool DrugVanSlashView::load(const std::string& dataDir) {
    dataDir_ = dataDir;
    sheetOk_ = false;
    bgOk_ = false;
    vanBg_ = vanOverlay_ = 0;
    sheet_ = 0;

    // The van backgrounds: the gGameDir[26] theme table from resource 0, then its 0xe1 / 0xe2 entries, loaded as
    // textures by resource id. Mirrors cSinkingCar::start reading gGameDir[26] for its interior.
    std::vector<uint8_t> directory, backgrounds, slots;
    if (Assets_Pak().read(0, directory) && directory.size() >= 27 * 2 &&
        Assets_Pak().read(tableEntry(directory, 26), backgrounds)) {
        vanBg_ = tableEntry(backgrounds, kVanBgId);
        vanOverlay_ = tableEntry(backgrounds, kVanOverlayId);
        // vanBg_ is required for a recognisable van; the overlay is optional chrome.
        bgOk_ = vanBg_ != 0 && vanBg_ != 0xFFFF && Assets_Texture(vanBg_) != 0;
    }

    // The slash / tear sprite sheet: SS_DrugVanSlashing.bin + its sheet texture through the gGameDir[23] slot table,
    // exactly as sinkingcar/hotwiring load their loose sheets. Absent in a minimal data dir -> degrade gracefully.
    GameFs fs;
    std::vector<uint8_t> defs;
    if (!directory.empty() && Assets_Pak().read(tableEntry(directory, 23), slots) && fs.open(dataDir) &&
        fs.read("SS_DrugVanSlashing.bin", defs) && sprites_.parse(defs) && !sprites_.sprites().empty()) {
        const int sheet = tableEntry(slots, sprites_.sprites()[0].sheetSlot + 1);
        sheet_ = Assets_Texture(sheet);
        sheetOk_ = sheet_ != 0 && Assets_TextureSize(sheet, &sheetW_, &sheetH_);
    }

    return bgOk_;
}

void DrugVanSlashView::start(drugvanslash::VanKind kind, int itemId, int qty, std::function<void(bool)> done) {
    game_.playSfx = playSfx;
    game_.awardStash = awardStash;
    game_.start(kind, itemId, qty, std::move(done));
    dragging_ = false;
    dragTravel_ = 0.f;
}

void DrugVanSlashView::stop() {
    game_.stop();
    dragging_ = false;
}

void DrugVanSlashView::process() {
    if (game_.active()) game_.process();
}

void DrugVanSlashView::toApp(int x, int y, int W, int H, float& ax, float& ay) const {
    ax = W > 0 ? x * 1024.f / W : 0.f;   // cSinkingCarApp::OnPress's plain 1024x768 stretch
    ay = H > 0 ? y * 768.f / H : 0.f;
}

// ---- input --------------------------------------------------------------------------------------------------
void DrugVanSlashView::onMouseDown(int x, int y, int W, int H) {
    if (!game_.active() || W <= 0 || H <= 0) return;
    Vec2 p;
    toApp(x, y, W, H, p.x, p.y);
    dragging_ = true;
    dragFrom_ = p;
    dragLast_ = p;
    dragTravel_ = 0.f;
}

// Offer the un-cut part of the current stroke (dragFrom_ -> p) to the logic. cSlash::Update (0x7f38b4) completes a
// slash once the one-way sweep covers the whole segment; here each accepted swipe advances the index. One coarse
// move event can span several door edges, so re-anchor the stroke at the corner just cut (the new segment's start
// waypoint) and keep offering the same finger point to the next segment until it no longer completes one.
void DrugVanSlashView::feedSwipe(const Vec2& p) {
    if (game_.state() != drugvanslash::State::Slashing) return;
    for (int guard = 0; guard < drugvanslash::kSlashCount; ++guard) {
        if (game_.state() != drugvanslash::State::Slashing || !game_.swipe(dragFrom_, p)) break;
        dragFrom_ = game_.segmentStart();   // the corner just completed == the next segment's start
    }
}

void DrugVanSlashView::onMouseMove(int x, int y, int W, int H) {
    if (!game_.active() || !dragging_ || W <= 0 || H <= 0) return;
    Vec2 p;
    toApp(x, y, W, H, p.x, p.y);
    dragTravel_ += dist(dragLast_, p);
    dragLast_ = p;
    feedSwipe(p);
}

void DrugVanSlashView::onMouseUp(int x, int y, int W, int H) {
    if (!game_.active() || W <= 0 || H <= 0) {
        dragging_ = false;
        return;
    }
    Vec2 p;
    toApp(x, y, W, H, p.x, p.y);
    dragTravel_ += dist(dragLast_, p);
    dragLast_ = p;
    if (game_.state() == drugvanslash::State::Slashing)
        feedSwipe(p);
    else if (game_.state() == drugvanslash::State::Grabbing && dragTravel_ < 12.f)
        game_.grab();   // cOnRelease: a tap/release on the opened van
    dragging_ = false;
}

// ---- drawing ------------------------------------------------------------------------------------------------
void DrugVanSlashView::drawBackground(int texture, int W, int H) const {   // cBg::Render, plain 1024x768
    int tw = 1, th = 1;
    const unsigned gl = texture ? Assets_Texture(texture) : 0;
    if (!gl || !Assets_TextureSize(texture, &tw, &th)) return;
    (void)tw; (void)th;   // cContainerWnd::AddBgWindow fills the whole screen with the texture (UV 0..1), stretching
    const float kx = W / 1024.f, ky = H / 768.f;   // it regardless of its native size; draw it over the full app 1024x768
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, gl);
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(1, 0); glVertex2f(1024.f * kx, 0);
    glTexCoord2f(1, 1); glVertex2f(1024.f * kx, 768.f * ky);
    glTexCoord2f(0, 1); glVertex2f(0, 768.f * ky);
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

// One SS_DrugVanSlashing sprite, like cSinkingCar::sprite (the sheet is a game.pak texture, texels in its own space).
void DrugVanSlashView::drawSheetSprite(int index, float x, float y, float scale, float alpha, int W, int H) const {
    if (!sheetOk_ || index < 0 || index >= (int)sprites_.sprites().size()) return;
    const SpriteDef& d = sprites_.sprites()[index];
    const float kx = W / 1024.f, ky = H / 768.f;
    const float w = d.w + 1.f, h = d.h + 1.f;
    const float vx[4] = {-(float)d.offX, w - d.offX, w - d.offX, -(float)d.offX};
    const float vy[4] = {-(float)d.offY, -(float)d.offY, h - d.offY, h - d.offY};
    const float u[4] = {(float)d.x, d.x + w, d.x + w, (float)d.x};
    const float v[4] = {(float)d.y, (float)d.y, d.y + h, d.y + h};
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, sheet_);
    glColor4f(1, 1, 1, alpha);
    glBegin(GL_QUADS);
    for (int i = 0; i < 4; ++i) {
        glTexCoord2f(u[i] / sheetW_, v[i] / sheetH_);
        glVertex2f((x + vx[i] * scale) * kx, (y + vy[i] * scale) * ky);
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

// A flat-coloured line quad in app space (geometry, not art): the player's cut and the segment guides.
void DrugVanSlashView::drawLine(const Vec2& a, const Vec2& b, float width, uint32_t rgba, int W, int H) const {
    const float kx = W / 1024.f, ky = H / 768.f;
    float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-3f) return;
    const float nx = -dy / len * (width * 0.5f), ny = dx / len * (width * 0.5f);
    glColor4f((rgba >> 24 & 0xFF) / 255.f, (rgba >> 16 & 0xFF) / 255.f, (rgba >> 8 & 0xFF) / 255.f,
              (rgba & 0xFF) / 255.f);
    glBegin(GL_QUADS);
    glVertex2f((a.x + nx) * kx, (a.y + ny) * ky);
    glVertex2f((b.x + nx) * kx, (b.y + ny) * ky);
    glVertex2f((b.x - nx) * kx, (b.y - ny) * ky);
    glVertex2f((a.x - nx) * kx, (a.y - ny) * ky);
    glEnd();
}

void DrugVanSlashView::render(int W, int H) const {
    if (!game_.active() || W <= 0 || H <= 0) return;
    Hud_Begin(W, H);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    Hud_Rect(0.f, 0.f, (float)W, (float)H, 0x000000FFu);   // opaque backdrop so the live world never shows through

    // The real van art: the background (0xe1) then the overlay (0xe2) over the full screen (both AddBgWindow'd).
    if (bgOk_) drawBackground(vanBg_, W, H);
    if (vanOverlay_ && vanOverlay_ != 0xFFFF) drawBackground(vanOverlay_, W, H);

    const int done = game_.slashesDone();
    const int need = game_.slashesNeeded();

    // The slashes already cut: the real slash sprite along each segment when the sheet loaded, otherwise a line.
    for (int i = 0; i < done && i < need; ++i) {
        const Vec2 a = drugvanslash::kWaypoints[i], b = drugvanslash::kWaypoints[i + 1];
        drawLine(a, b, 6.f, 0xFFF2B0FFu, W, H);   // the cut itself is geometry
        if (sheetOk_)                              // the real blade sprite, centred on the cut
            drawSheetSprite(kSlashSprite[i], (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, 1.f, 1.f, W, H);
    }

    if (game_.state() == drugvanslash::State::Slashing) {
        // The current segment's guide, drawn clearly so the player sees the edge to cut and the direction (a bright
        // marker at the start waypoint, the blade sprite dimmed on the segment), plus the live swipe being traced.
        const Vec2 a = game_.segmentStart(), b = game_.segmentEnd();
        drawLine(a, b, 4.f, 0xFFFFFFAAu, W, H);
        drawLine(a, {a.x + (b.x - a.x) * 0.12f, a.y + (b.y - a.y) * 0.12f}, 10.f, 0x60FF60FFu, W, H);  // start cue
        if (sheetOk_ && done < drugvanslash::kSlashCount)
            drawSheetSprite(kSlashSprite[done], (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, 1.f, 0.45f, W, H);
        if (dragging_) drawLine(dragFrom_, dragLast_, 5.f, 0x80FFFFFFu, W, H);
    } else {
        // Tearing / grabbing / result: the doors part. tearProgress() drives the gap; the real SS_DrugVanSlashing
        // blade/tear sprites ride the two parting halves (left door pushed left, right door right, about the door
        // centre ~842). Without the sheet this degrades to the two guide edges parting; never fabricated van art.
        const drugvanslash::State st = game_.state();
        const bool opened = st == drugvanslash::State::Grabbing || st == drugvanslash::State::Result;
        const float t = game_.tearProgress();
        const float open = std::min(1.f, 0.5f * (t + (opened ? 1.f : 0.f))) * 40.f;   // px the cut edges part
        for (int i = 0; i < need; ++i) {
            const Vec2 wa = drugvanslash::kWaypoints[i], wb = drugvanslash::kWaypoints[i + 1];
            const float cx = (wa.x + wb.x) * 0.5f, cy = (wa.y + wb.y) * 0.5f;
            const float push = cx > 842.f ? open : -open;   // split about the door centre (~842)
            Vec2 a = wa, b = wb;
            a.x += push; b.x += push;
            drawLine(a, b, 6.f, 0xFFF2B0FFu, W, H);
            if (sheetOk_)   // the real tear/blade sprite riding this parting edge
                drawSheetSprite(kSlashSprite[i], cx + push, cy, 1.f, 1.f, W, H);
        }
    }

    // Chrome: the state line (Hud_Text is primitive chrome, allowed).
    const char* label =
        game_.state() == drugvanslash::State::Slashing ? "Slash the doors open" :
        game_.state() == drugvanslash::State::TearingOpen ? "Tearing open..." :
        game_.state() == drugvanslash::State::Grabbing ? "Tap to grab the stash" : "Done";
    Hud_Text(24.f, 24.f, 1.f, 0xFFFFFFFFu, label);
    if (game_.state() == drugvanslash::State::Slashing) {
        char prog[32];
        std::snprintf(prog, sizeof(prog), "Slash %d / %d", std::min(done + 1, need), need);
        Hud_Text(24.f, 48.f, 1.f, 0xFFFFFFFFu, prog);
    }
    if (!sheetOk_)
        Hud_Text(24.f, 72.f, 1.f, 0xFFB0B0FFu, "SS_DrugVanSlashing.bin not loaded - cuts drawn as lines");

    Hud_End();
}
