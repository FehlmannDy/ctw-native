// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
#include "safecrackingview.h"
#include "hud.h"
#include "gfx/assets.h"
#include "os/gamefs.h"
#include "os/pak.h"
#include <glad/gl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr float kTwoPi = 6.2831853f;

// cSafeCrackingApp::Init (0x81c718). The sprite SET indices and app-space anchors, observed from the AddSpriteImpl /
// AddInteractiveSpriteWindow calls and ShowNumbers (0x81e708); marked observation / assumption in the header.
constexpr int   kSlotTableEntry = 23;        // gGameDir[23]: the sprite-sheet slot table resource (as HotWiring does)
constexpr int   kBgTableEntry   = 26;        // gGameDir[26]: the PDA theme background table (as the drug van uses)
constexpr int   kSafeBgId       = 0x6e;      // Init 0x81c8d0: AddBgWindow bg-table id for the safe body (observation)
constexpr int   kDialSetIndex   = 21;        // meta +0x150 -> set 21, the sprite SetSpriteRotation drives (Init 0x81cc20)
constexpr float kDialPivotX     = 818.f;     // ProcessAnalogRotation 0x81e594: touch - 0x332 (observation)
constexpr float kDialPivotY     = 332.f;     // ProcessAnalogRotation 0x81e594: touch - 0x14c (observation)
constexpr float kDigitX         = 500.f;     // ShowNumbers digit sprites at x = 0x1f4 (observation)
constexpr float kDigitY         = 563.f;     // ... y = 0x233 (Init 0x81cd94, observation)
constexpr int   kStethoFirst    = 2;         // stetho bars: meta 0x20..0x90 -> set indices 2..9 (Init loop 0x81ccf0)
constexpr int   kStethoCount    = 8;         // eight bars (kStethoMax is 7; bar 0 is the floor)
constexpr float kStethoX        = 500.f;     // x = 0x1f4 (observation); the per-bar y is a run-time table (assumption)
constexpr float kStethoTopY     = 470.f;     // stacked base (assumption: the real y table is .bss 0xdd0660, not static)
constexpr float kStethoBarStep  = 10.f;      // (assumption)

// cSafeCrackingApp tints the combination digits via cSprite+0x74 (ShowNumbers 0x81e708): the two 32-bit packed-RGBA
// constants -0xffff01 = 0xFF0000FF and -0xff0100 = 0xFF00FF00, i.e. RED and GREEN. Expressed here as 0xAARRGGBB.
constexpr uint32_t kDigitRed    = 0xFFFF0000u;   // searching (not yet on the target notch)
constexpr uint32_t kDigitGreen  = 0xFF00FF00u;   // on the target notch / locked in
constexpr uint32_t kWhite       = 0xFFFFFFFFu;
constexpr uint32_t kDim         = 0x40FFFFFFu;    // an unlit stetho bar (alpha 0x40)

uint16_t tableEntry(const std::vector<uint8_t>& t, int i) {
    uint16_t v = 0;
    if ((size_t)(i + 1) * 2 <= t.size()) std::memcpy(&v, t.data() + i * 2, 2);
    return v;
}

// shortest signed difference b - a wrapped to (-pi, pi]
float angleDelta(float a, float b) {
    float d = b - a;
    while (d > 3.14159265f) d -= kTwoPi;
    while (d < -3.14159265f) d += kTwoPi;
    return d;
}
}  // namespace

// cSafeCrackingApp::RenderBg (0x81eef4) issues SetOrthoMatrix(0, 1024, 0, 768, -1, 1) and Render (0x81f0bc) draws the
// background over the whole (0,0)->(1024,768): the app is FULL-SCREEN 1024x768 (like cSinkingCarApp), not right-
// aligned like HotWiring, so there is no horizontal ortho correction.
float SafeCrackingView::appLeft() const { return 0.f; }

void SafeCrackingView::toApp(int x, int y, float& ax, float& ay) const {
    if (W_ <= 0 || H_ <= 0) { ax = ay = 0; return; }
    const float left = appLeft();
    ax = left + x * (1024.f - left) / W_;
    ay = y * 768.f / H_;
}

bool SafeCrackingView::load(const std::string& dataDir) {
    loaded_ = false;
    bgOk_ = false;
    sheet_ = 0;
    bg_ = 0;
    // Mirror SinkingCar::start / HotWiring::start: the sheet texture's game.pak slot from gGameDir[23], and the
    // sprite definitions from the WAD through GameFs. All art is the user's own; nothing is fabricated here.
    std::vector<uint8_t> directory, slots, defs, backgrounds;
    GameFs fs;
    if (!Assets_Pak().read(0, directory) || directory.size() < 27 * 2) return false;

    // The full-screen safe body: the gGameDir[26] PDA-theme background table, then its 0x6e entry, loaded as a
    // texture by resource id -- exactly as DrugVanSlashView loads van backgrounds 0xe1/0xe2. REAL safe art, not drawn.
    if (Assets_Pak().read(tableEntry(directory, kBgTableEntry), backgrounds)) {
        bg_ = tableEntry(backgrounds, kSafeBgId);
        bgOk_ = bg_ != 0 && bg_ != 0xFFFF && Assets_Texture(bg_) != 0;
    }

    if (!Assets_Pak().read(tableEntry(directory, kSlotTableEntry), slots)) return false;
    if (!fs.open(dataDir) || !fs.read("SS_SAFECRACKING.bin", defs) || !sprites_.parse(defs)) return false;
    // The dial (21) and the digits (notch + 0xb, up to 20) must exist in the set.
    if (sprites_.sprites().size() < 21u + 1u) return false;
    const int sheet = tableEntry(slots, sprites_.sprites()[0].sheetSlot + 1);
    sheet_ = Assets_Texture(sheet);
    if (!sheet_ || !Assets_TextureSize(sheet, &sheetW_, &sheetH_)) { sheet_ = 0; return false; }
    loaded_ = true;
    return true;
}

// cSprite::CalculateVerts, as HotWiring::drawSprite: the set sprite (w+1 by h+1) about its stored offX/offY handle,
// turned by the 16-bit rotation, mapped from app 1024x768 (mode 2) into window pixels.
void SafeCrackingView::drawSprite(int setIndex, float ax, float ay, uint16_t rotation, uint32_t rgba) const {
    const float A = ((rgba >> 24) & 0xFF) / 255.f;
    if (!sheet_ || setIndex < 0 || setIndex >= (int)sprites_.sprites().size() || A <= 0.f) return;
    const float R = ((rgba >> 16) & 0xFF) / 255.f, G = ((rgba >> 8) & 0xFF) / 255.f, B = (rgba & 0xFF) / 255.f;
    const SpriteDef& d = sprites_.sprites()[setIndex];
    const float left = appLeft(), kx = W_ / (1024.f - left), ky = H_ / 768.f;
    const float w = d.w + 1.f, h = d.h + 1.f;
    const float a = rotation * kTwoPi / 65536.f, c = std::cos(a), sn = std::sin(a);
    const float vx[4] = {-(float)d.offX, w - d.offX, w - d.offX, -(float)d.offX};
    const float vy[4] = {-(float)d.offY, -(float)d.offY, h - d.offY, h - d.offY};
    const float u[4] = {(float)d.x, d.x + w, d.x + w, (float)d.x};
    const float v[4] = {(float)d.y, (float)d.y, d.y + h, d.y + h};
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, sheet_);
    glColor4f(R, G, B, A);
    glBegin(GL_QUADS);
    for (int i = 0; i < 4; ++i) {
        const float px = vx[i] * c + vy[i] * sn, py = -vx[i] * sn + vy[i] * c;
        glTexCoord2f(u[i] / sheetW_, v[i] / sheetH_);
        glVertex2f((ax + px - left) * kx, (ay + py) * ky);
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

// cBg::Render: the AddBgWindow safe body stretched over the full 1024x768 window (RenderBg 0x81eef4 draws one quad
// whose texture is the resolved background). Mirrors DrugVanSlashView::drawBackground: real art, by resource id.
void SafeCrackingView::drawBackground(unsigned texture) const {
    int tw = 1, th = 1;
    const unsigned gl = texture ? Assets_Texture(texture) : 0;
    if (!gl || !Assets_TextureSize(texture, &tw, &th)) return;
    (void)tw; (void)th;   // RenderBg fills the whole screen with the texture (UV 0..1); draw it over the full app 1024x768
    const float kx = W_ / 1024.f, ky = H_ / 768.f;
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

void SafeCrackingView::render(int W, int H) {
    W_ = W;
    H_ = H;
    if (W <= 0 || H <= 0) return;
    Hud_Begin(W, H);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    Hud_Rect(0.f, 0.f, (float)W, (float)H, 0x000000FFu);   // opaque backdrop: the minigame replaces the view (the
    // original fades the world to near-black), so the live world never shows through the art's transparent areas

    const float left = appLeft(), kx = W / (1024.f - left), ky = H / 768.f;
    auto px = [&](float appX) { return (appX - left) * kx; };
    auto py = [&](float appY) { return appY * ky; };

    // --- the full-screen safe BODY (AddBgWindow 0x6e), behind everything; grey chrome only if the art is absent --
    if (bgOk_)
        drawBackground(bg_);
    else
        Hud_Rect(px(360.f), py(150.f), (640.f) * kx, (520.f) * ky, 0x000000A0u);

    if (!loaded_) {
        // The dial/panel sheet is missing: say so in chrome over whatever background loaded, and stop.
        Hud_Text(px(380.f), py(360.f), 1.0f, 0xFFFFFFFFu, "SS_SAFECRACKING sheet not loaded");
        Hud_End();
        return;
    }

    // --- real loaded sprites ------------------------------------------------------------------------------------
    // The stethoscope bars (set 2..9): light bars 0..stethoLevel() (the game forces 7 at the exact notch).
    const int level = std::clamp(game_.stethoLevel(), 0, kStethoCount - 1);
    for (int i = 0; i < kStethoCount; ++i)
        drawSprite(kStethoFirst + i, kStethoX, kStethoTopY - i * kStethoBarStep, 0, i <= level ? kWhite : kDim);
    // The rotating dial face (set 21) about its pivot, turned by the live dial rotation.
    drawSprite(kDialSetIndex, kDialPivotX, kDialPivotY, (uint16_t)(int16_t)game_.rotation(), kWhite);
    // The combination readout (set notch + 0xb): the digit the dial currently points at, tinted the real red/green
    // the game writes to cSprite+0x74 -- green once the notch sits on the target (atTarget), red while searching.
    const int notch = game_.currentNotch();
    drawSprite(safecracking::SafeCracking::getDigitalNumberSpriteId(notch), kDigitX, kDigitY, 0,
               game_.atTarget() ? kDigitGreen : kDigitRed);

    // --- chrome labels (not art): title, tumbler progress, state ------------------------------------------------
    Hud_Text(px(380.f), py(165.f), 1.0f, 0xFFFFFFFFu, "CRACK THE SAFE");
    const int step = game_.step();
    char line[64];
    if (step >= safecracking::kStepTumbler0 && step <= safecracking::kStepTumbler2) {
        std::snprintf(line, sizeof line, "Tumbler %d / 3", game_.tumbler() + 1);
    } else if (game_.finished()) {
        std::snprintf(line, sizeof line, "OPEN");
    } else {
        std::snprintf(line, sizeof line, "...");
    }
    Hud_Text(px(380.f), py(630.f), 1.0f, 0xFFFF00FFu, line);

    Hud_End();
}

// cInteractiveSprite grab: record the cursor's angle about the dial pivot.
void SafeCrackingView::onMouseDown(int x, int y) {
    if (!loaded_) return;
    float ax, ay;
    toApp(x, y, ax, ay);
    dragAngle_ = std::atan2(ay - kDialPivotY, ax - kDialPivotX);
    dragging_ = true;
}

// ProcessAnalogRotation 0x81e594: the swept angle between the old and new cursor lines turns the dial. The binary
// accumulates it as rotation -= GetAngleBetweenLines(oldVec @+0x190, newVec)  (0x81e6f0 fsub: s_rot = s_rot - angle),
// i.e. the stored int16 rotation DECREASES for a CCW finger sweep. The view's atan2 delta runs the opposite way, so
// it is negated here to turn the dial the way the game does (fixes the user's "dial turns in reverse").
void SafeCrackingView::onMouseMove(int x, int y) {
    if (!loaded_ || !dragging_) return;
    float ax, ay;
    toApp(x, y, ax, ay);
    const float now = std::atan2(ay - kDialPivotY, ax - kDialPivotX);
    const float d = angleDelta(dragAngle_, now);
    game_.rotateBy(-(int)std::lround(d * 65536.f / kTwoPi));
    dragAngle_ = now;
}

// cSafeCrackingApp::OnRelease: let go of the dial. The logic locks the tumbler on the exact notch, else a free retry.
void SafeCrackingView::onMouseUp(int x, int y) {
    if (!loaded_) return;
    if (dragging_) onMouseMove(x, y);
    dragging_ = false;
    game_.onRelease();
}
