// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
#include "pda.h"

#include "hud.h"
#include "gfx/spriteset.h"
#include "gfx/assets.h"      // engine resource/texture loader (loadBody: the DXT phone bezel from game.pak)
#include "os/datafile.h"
#include "os/gamefs.h"
#include "os/pak.h"          // Assets_Pak().read() for the theme texture-id table resource
#include <glad/gl.h>
#include <stb_image.h>
#include <cstdio>
#include <vector>

namespace pda {

namespace {
// Draw one SS_AlwaysLoaded sprite by its index, reproducing GameplayHud::sprite (gamehud.cpp): the loose
// ss_alwaysloaded.png is 2x the sheet's layout space (SpriteSet::pngScaleFor), so texels are the stored
// coordinate doubled; vertices span the sprite's own size about its stored offX/offY handle, in the HUD's
// top-left pixel space. No rotation is needed for the full-screen PDA background.
void drawAlwaysLoadedSprite(const SpriteSet& set, unsigned texture, int texW, int texH, int index,
                            float x, float y, float scale, float alpha, uint32_t tint) {
    if (texture == 0 || texW <= 0 || texH <= 0) return;
    if (index < 0 || index >= (int)set.sprites().size()) return;
    const SpriteDef& d = set.sprites()[index];
    const float w = d.w + 1.f, h = d.h + 1.f;   // cSprite::CalculateVerts includes the last stored pixel
    if ((d.x + w) * 2 > texW || (d.y + h) * 2 > texH) return;
    const float vx[4] = {-(float)d.offX, w - d.offX, w - d.offX, -(float)d.offX};
    const float vy[4] = {-(float)d.offY, -(float)d.offY, h - d.offY, h - d.offY};
    const float u[4] = {d.x * 2.f, (d.x + w) * 2.f, (d.x + w) * 2.f, d.x * 2.f};
    const float v[4] = {d.y * 2.f, d.y * 2.f, (d.y + h) * 2.f, (d.y + h) * 2.f};
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    glColor4f((tint >> 24) / 255.f, (tint >> 16 & 0xFF) / 255.f, (tint >> 8 & 0xFF) / 255.f,
              (tint & 0xFF) / 255.f * alpha);
    glBegin(GL_QUADS);
    for (int i = 0; i < 4; ++i) {
        glTexCoord2f(u[i] / texW, v[i] / texH);
        glVertex2f(x + vx[i] * scale, y + vy[i] * scale);
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}
}  // namespace

// ---------------------------------------------------------------------------
// PdaApp: cPdaApp's shared lifecycle (cPdaApp::ProcessAllStates, 0x7a22bc caller).
// The app's +0x138 state advances Loading -> Appearing -> Running, and Leaving -> finished.
// +0x168 (busy_) stays set until it settles into Running, which IsTransitioning / LoadApp test.
// ---------------------------------------------------------------------------
void PdaApp::processAllStates() {
    switch (state_) {
    case AppState::Loading:
        busy_ = true;
        if (onLoad())
            state_ = AppState::Appearing;
        break;
    case AppState::Appearing:
        busy_ = true;
        if (onAppear()) {
            state_ = AppState::Running;
            busy_ = false;   // +0x168 cleared: now Running
        }
        break;
    case AppState::Running:
        onProcess();
        break;
    case AppState::Leaving:
        busy_ = true;
        if (!finished_ && onLeave())
            finished_ = true;   // the outro is done; cPda::SwitchApp drops the app, not the app itself
        break;
    }
}

void PdaApp::transitionHeadingOff() {
    if (state_ != AppState::Leaving) {
        state_ = AppState::Leaving;
        busy_ = true;
    }
}

// ---------------------------------------------------------------------------
// PdaTransition cover: 0 at the ends, 1 across the middle. WhiteFade / LoadingAnimation obscure the swap at the
// midpoint; Instant never covers (it swaps with no animation).
// ---------------------------------------------------------------------------
float PdaTransition::cover() const {
    if (kind_ == Kind::Instant || total_ <= 0)
        return 0.f;
    float half = total_ * 0.5f;
    float d = frame_ <= half ? frame_ / half : (total_ - frame_) / half;
    if (d < 0.f) d = 0.f;
    if (d > 1.f) d = 1.f;
    return d;
}

// ---------------------------------------------------------------------------
// cPda::Init (0x79fe24): sets the shell up. The original also builds the clock text sprite (+0x1700) and the
// background sprite (+0x1668) from SS_AlwaysLoaded 0x79 and calls RefreshDaySprite; those are asset work left to
// the hosting layer (documented in the header). The pure shell state starts cleared.
// ---------------------------------------------------------------------------
void Pda::init() {
    app_.reset();
    transition_.reset();
    targetApp_ = kAppNone;
    pendingApp_ = kAppNone;
    pendingSwitch_ = false;
    enabled_ = false;
    inputDelay_ = 0;
    deferredApp_ = -1;
    flashMinute_ = -1;
    clockFlash_ = false;
    loadingState_ = 0;
    loadingAlpha_ = 0.f;
    loadingShown_ = false;
    homeOpen_ = false;
    gallerySel_ = 0;
}

// cPda::Enable (0x7a3b5c): +0x1735 = on & HaveShownStings().
void Pda::enable(bool on, bool haveShownStings) {
    enabled_ = on && haveShownStings;
}

// ---------------------------------------------------------------------------
// cPda::LoadApp (0x7a2db8): a large dispatch from the target app id to a specific transition flavour
// (cInstantNoFadeTransition / cPdaTransWhiteFade / cPdaTransLoadingAnimation). Faithfully reproduced here as:
//   - early-out if the same app is already the resolved target and settled (0x7a2e50);
//   - tear down any live transition and tell the current app to head off (0x7a2e7c..0x7a2ea0);
//   - choose a transition kind for the app id, resolve +0x1660, arm the pending switch (+0x1736), which
//     cTaskBar::PushPreviousAppStack would also record.
// The exact per-id transition table (white fade vs loading animation vs instant) is game data left to the apps;
// the shell picks a sensible default by id and documents it. (observation: the structure; assumption: the default
// kind per id, since the full ePdaApp table is out of scope.)
// ---------------------------------------------------------------------------
void Pda::loadApp(int appId, ResetOption reset) {
    // 0x7a2e50: already resolved to this app and not mid-transition -> nothing to do.
    if (targetApp_ == appId && !transition_ &&
        (!app_ || (app_->state() == AppState::Running && !app_->busy())))
        return;

    // Drop a live transition and ask the running app to leave (TransitionHeadingOff, 0x7a2ea0).
    transition_.reset();
    if (app_)
        app_->transitionHeadingOff();

    // Pick the transition. The home/HUD app (0x10) swaps instantly in the original's common paths; other apps
    // fade in. This mirrors the roles without baking the full per-id table.
    PdaTransition::Kind kind = (appId == kAppHud) ? PdaTransition::Kind::Instant
                                                  : PdaTransition::Kind::WhiteFade;
    // An instant transition has no span (reports not-active at once); a fade plays over ten frames.
    transition_ = std::make_unique<PdaTransition>(kind, kind == PdaTransition::Kind::Instant ? 0 : 10);

    targetApp_ = appId;          // +0x1660
    pendingApp_ = appId;
    pendingReset_ = reset;       // stored for the factory
    pendingSwitch_ = true;       // +0x1736

    // An instant transition has no covered midpoint, so swap immediately.
    if (kind == PdaTransition::Kind::Instant)
        switchApp(reset == ResetOption::Reset);
}

void Pda::deferLoadApp(int appId, ResetOption reset) {
    deferredApp_ = appId;        // +0x176c
    deferredReset_ = reset;      // +0x1770
}

// ---------------------------------------------------------------------------
// cPda::SwitchApp (called from ProcessLoadApp): drop the old app and build the pending one via the factory.
// ---------------------------------------------------------------------------
void Pda::switchApp(bool reset) {
    if (!pendingSwitch_)
        return;
    pendingSwitch_ = false;
    app_.reset();
    if (pendingApp_ != kAppNone && appFactory)
        app_ = appFactory(pendingApp_, reset ? ResetOption::Reset : pendingReset_);
}

// cPda::ProcessLoadApp (0x7a3b94): if an app is still present and `appStillRunning`, wait; otherwise SwitchApp.
void Pda::processLoadApp(bool appStillRunning, bool reset) {
    if (app_ && appStillRunning)
        return;
    switchApp(reset);
}

// Step the transition and run the midpoint switch (cPda::ProcessTransitions' role).
void Pda::processTransitions() {
    if (!transition_)
        return;
    transition_->process();
    // At the covered midpoint, swap the app under cover (ProcessLoadApp -> SwitchApp). The outgoing app has been
    // asked to leave, so it is not "still running": the swap always fires here.
    if (pendingSwitch_ && transition_->atMidpoint())
        processLoadApp(false, pendingReset_ == ResetOption::Reset);
    if (!transition_->isActive())
        transition_.reset();
}

// ---------------------------------------------------------------------------
// cPda::IsAppRunning (0x7a21bc): no app, or the app settled into Running (+0x138 == 2).
// ---------------------------------------------------------------------------
bool Pda::isAppRunning() const {
    return !app_ || app_->state() == AppState::Running;
}

// cPda::IsTransitioning (0x7a25a4): a live transition, or an app not yet settled (not Running, or still busy).
bool Pda::isTransitioning() const {
    if (transition_ && transition_->isActive())
        return true;
    if (app_ && (app_->state() != AppState::Running || app_->busy()))
        return true;
    return false;
}

// ---------------------------------------------------------------------------
// cPda::ProcessDeferedAppLoading (0x7a27a4): a queued app id (+0x176c, -1 none) fires once the current app has
// settled into Running (and is not the special "no interrupt" app 0x1f). Then LoadApp runs and the queue clears.
// ---------------------------------------------------------------------------
void Pda::processDeferedAppLoading() {
    if (deferredApp_ == -1)
        return;
    // Wait while the app is still appearing/leaving or a transition is active (0x7a27d4..0x7a2838).
    if (app_ && (app_->state() != AppState::Running || app_->busy()))
        return;
    if (transition_ && transition_->isActive())
        return;
    int appId = deferredApp_;
    ResetOption reset = deferredReset_;
    deferredApp_ = -1;           // 0x7a2888
    loadApp(appId, reset);
}

// ---------------------------------------------------------------------------
// cPda::FlashClock (0x7a0f90): the clock flashes on a minute change while the HUD (0x10) or map (0x4a) app is up.
// The original schedules the next tick through a cWeakProxy/cCallBack (deferred; see the header); the observable
// result is the +0x1768 flag that flips when the shown minute advances.
// ---------------------------------------------------------------------------
bool Pda::flashClock(int minute, int hour) {
    (void)hour;
    bool hudOrMap = app_ && (app_->id() == kAppHud || app_->id() == kAppMap);
    if (!hudOrMap) {
        clockFlash_ = false;
        flashMinute_ = minute;
        return false;
    }
    if (minute != flashMinute_) {
        flashMinute_ = minute;
        clockFlash_ = true;      // +0x1768 set on the tick
    } else {
        clockFlash_ = false;
    }
    return clockFlash_;
}

// ---------------------------------------------------------------------------
// cPda::DisplayLoading (0x7a5020): show -> create the overlay sprite (+0x17d0), SetShowInPause(true), alpha 0,
// state (+0x17c8) = 1 (fade in). hide -> state = 2 (fade out, then removed). The sprite art is asset work.
// ---------------------------------------------------------------------------
void Pda::displayLoading(bool show) {
    if (show) {
        loadingShown_ = true;
        loadingAlpha_ = 0.f;
        loadingState_ = 1;
    } else {
        loadingState_ = 2;
    }
}

// The fade stepped each frame in cPda::Process (0x7a2314..0x7a23e4): state 1 ramps +0.1 up to 0.5; state 2 ramps
// -0.1 down to 0, then clears the state and removes the sprite. Step constant 0.1 (0x3dcccccd).
void Pda::updateLoadingOverlay() {
    if (loadingState_ == 1) {
        loadingAlpha_ += 0.1f;
        if (loadingAlpha_ >= 0.5f) {
            loadingAlpha_ = 0.5f;
            loadingState_ = 0;
        }
    } else if (loadingState_ == 2) {
        loadingAlpha_ -= 0.1f;
        if (loadingAlpha_ <= 0.f) {
            loadingAlpha_ = 0.f;
            loadingState_ = 0;
            loadingShown_ = false;   // the sprite is removed (RemoveSprite)
        }
    }
}

// ---------------------------------------------------------------------------
// cPda::ProcessInput (0x7a25f8): swallow input while the open delay (+0x1730) runs down, then route the touch to
// the hosted app when it is interactive (Appearing/Running). The original also routes to cTaskBar (home / back
// buttons); the taskbar is a separate port concern, so the shell forwards the raw touch to the app.
// ---------------------------------------------------------------------------
void Pda::processInput(const Touch& touch) {
    if (enabled_ && inputDelay_ > 0) {
        --inputDelay_;
        return;
    }
    if (!app_)
        return;
    if (app_->state() == AppState::Appearing || app_->state() == AppState::Running)
        app_->onInput(touch);
}

// ---------------------------------------------------------------------------
// cPda::Process (0x7a21e8): one frame. Step the transition, run the app's state machine (routing input first when
// interactive), run defered loading, flash the clock and step the loading overlay. The autosave hook (+0x1746),
// screen saver (+0x1678), help/email/vigilante managers are separate ports and left out of this shell.
// ---------------------------------------------------------------------------
void Pda::process(int minute, int hour) {
    if (transition_)
        processTransitions();

    if (app_) {
        // ProcessInput runs when the app is interactive (state in {Appearing, Running}); then ProcessAllStates.
        // The shell routes the live touch through processInput() separately each frame, so here we only advance
        // the app's own state machine.
        app_->processAllStates();
        // The app is never deleted here: cPda::SwitchApp (at the transition midpoint) replaces it. An app left in
        // its finished Leaving state simply waits for that swap, as in the original.
    }

    processDeferedAppLoading();

    clockMinute_ = minute;   // captured for the clock text sprite (+0x1700), drawn in render()
    clockHour_ = hour;
    flashClock(minute, hour);
    updateLoadingOverlay();
}

// ---------------------------------------------------------------------------
// cPda::DrawScreenRect (0x7a4e94): a filled quad (triangle strip). The original forces the GL colour white and
// stores only the alpha byte (colour>>24) per vertex, so it is a white rectangle at that alpha, used by the fade
// transitions. The port keeps the colour parameter but draws faithfully (white, alpha = colour>>24).
// ---------------------------------------------------------------------------
void Pda::drawScreenRect(uint32_t colour, int x0, int y0, int x1, int y1) {
    uint32_t alpha = (colour >> 24) & 0xFF;
    uint32_t rgba = 0xFFFFFF00u | alpha;   // white, alpha-only, as the binary draws it
    float x = static_cast<float>(x0 < x1 ? x0 : x1);
    float y = static_cast<float>(y0 < y1 ? y0 : y1);
    float w = static_cast<float>(x0 < x1 ? x1 - x0 : x0 - x1);
    float h = static_cast<float>(y0 < y1 ? y1 - y0 : y0 - y1);
    Hud_Rect(x, y, w, h, rgba);
}

void Pda::setFrameSprites(const SpriteSet* sheet, unsigned texture, int texWidth, int texHeight) {
    frameSheet_ = sheet;
    frameTexture_ = texture;
    frameTexW_ = texWidth;
    frameTexH_ = texHeight;
}

void Pda::setScreenMetrics(int screenWidth, int screenHeight) {
    screenW_ = screenWidth;
    screenH_ = screenHeight;
}

// ---------------------------------------------------------------------------
// Load the PDA chrome atlas the same way GameplayHud::init (gamehud.cpp) loads SS_AlwaysLoaded: pull the sprite
// definitions from "SS_PdaSprites.bin" through GameFs (which resolves .bin from ROM.WAD), parse them into a
// SpriteSet, read the loose "ss_pdasprites.png", upload it as an RGBA GL texture with the same filtering/clamp
// the HUD uses, and keep the sheet + texture + pixel size in the shell.
//
// SS_PdaSprites is cTaskBar's set (cTaskBar::AllocatePdaSpriteSet, 0x7ca04c, name string at 0x472a57). The png is
// 2048x512; at the SS_ 2x convention (SpriteSet::pngScaleFor) that is a 1024x256 layout. The sheet holds 34
// sprites: the per-app title banners and the home/back task-bar buttons (cTaskBar::Render), i.e. the PDA's UI
// chrome -- it does NOT contain a full phone-device frame (see the header note), so the loader only makes the real
// atlas available; render() keeps drawing the authentic +0x1668 backdrop (SS_AlwaysLoaded id 0x79).
bool Pda::loadTheme(const std::string& dataDir) {
    // Drop any previously loaded texture so a re-load does not leak.
    if (pdaTexture_) glDeleteTextures(1, &pdaTexture_);
    pdaTexture_ = 0;
    pdaTexW_ = pdaTexH_ = 0;
    pdaSheet_ = SpriteSet{};

    GameFs fs;
    std::vector<uint8_t> defs, png;
    if (!fs.open(dataDir) || !fs.read("SS_PdaSprites.bin", defs) || !pdaSheet_.parse(defs) ||
        !Data_ReadAll(dataDir + "/ss_pdasprites.png", png)) {
        pdaSheet_ = SpriteSet{};
        return false;
    }
    int channels;
    unsigned char* pixels = stbi_load_from_memory(png.data(), (int)png.size(), &pdaTexW_, &pdaTexH_, &channels, 4);
    if (!pixels) {
        pdaSheet_ = SpriteSet{};
        pdaTexW_ = pdaTexH_ = 0;
        return false;
    }
    glGenTextures(1, &pdaTexture_);
    glBindTexture(GL_TEXTURE_2D, pdaTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pdaTexW_, pdaTexH_, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);
    return true;
}

void Pda::drawPdaSprite(int index, float x, float y, float scale, float alpha, uint32_t tint) const {
    drawAlwaysLoadedSprite(pdaSheet_, pdaTexture_, pdaTexW_, pdaTexH_, index, x, y, scale, alpha, tint);
}

// ---------------------------------------------------------------------------
// Load the PDA device body/frame (the phone bezel) through the engine's resource/texture loader, reproducing
// cThemeBadger::Init (0x7ca7d8) -> cPdaTheme::SetupBG(0x59) (0x7ab518):
//   0x7ab540  w1 = *(u16*)(*(0xdce000+0xa88) + 0x34)      the theme texture-id TABLE resource id (runtime value)
//   0x7ab548  x19 = cResourceManager::Load(w1)            -> the table bytes (a flat u16 array)
//   0x7ab550  w21 = ((u16*)x19)[0x59]                     = the bezel texture id
//   0x7ab574  cTextureManager::Load(w21)                  -> uploads the texture
// cTextureManager::Load (0x6d9ac4) keys the texture by that id as a game.pak resource id, which is exactly what the
// port's Assets_Texture() consumes (Assets_Texture resolves dxt.bin / game.pak and decodes DXT -- see texload.h).
// So: read the table resource with Assets_Pak().read(themeTableResId), take its little-endian u16 at bezelIndex,
// and load that texture id with Assets_Texture(). Nothing is extracted, embedded or hand-drawn: this decodes the
// user's own game.pak at run time. The GL texture belongs to the engine cache (not deleted by the shell).
bool Pda::loadBody(const std::string& dataDir, int themeTableResId, int bezelIndex) {
    (void)dataDir;   // Assets must already be open (Assets_Open(dataDir) runs in Game::init)
    bodyTexture_ = 0;
    if (themeTableResId < 0 || bezelIndex < 0)
        return false;

    // cResourceManager::Load(themeTableResId): the u16 texture-id table.
    std::vector<uint8_t> table;
    if (!Assets_Pak().read(static_cast<uint32_t>(themeTableResId), table) ||
        table.size() < static_cast<size_t>(bezelIndex) * 2 + 2)
        return false;

    // table[bezelIndex] (ldrh, little-endian) = the bezel texture id.
    uint16_t texId = static_cast<uint16_t>(table[bezelIndex * 2] | (table[bezelIndex * 2 + 1] << 8));
    if (texId == 0xFFFF)        // SetupBG's "no texture" sentinel (0x7ab588)
        return false;

    // cTextureManager::Load(texId): decode + upload via the engine texture cache.
    unsigned tex = Assets_Texture(static_cast<int>(texId));
    if (!tex)
        return false;
    bodyTexture_ = tex;         // non-owned: the engine cache owns the GL texture
    return true;
}

// cPda::RenderPhoneSurround (0x7a1b98) draws the bezel cTexture as a single textured quad (cGlVtx2dPT) over the
// 1024x768 PDA space. Decoded fully from the binary (0x7a1d5c..0x7a1e58):
//   * Horizontal extent is FIXED, not screen-dependent: x0 = 0x90 = 144 (0x7a1de0), x1 = 0x48c = 1164 (0x7a1d84).
//     The widescreen half-excess widens only the black letterbox quad, never these bezel verts.
//   * Vertical extent is an aspect-fit about the screen centre. aspect = cTexture::Width()/*(this+0x20)
//     (sdiv at 0x7a1d88), where Width() = 1<<widthLog2 (POT width, 0x6d92f4) and +0x20 = the texture's raw pixel
//     height (cTexture::OnLoad 0x6d93d8 stores the image header's height there). halfExtent = (1023.0/aspect)*0.5
//     (0x447f0000 = 1023 at 0x7a1dbc, *0.5 at 0x7a1dcc..0x7a1dd0).
//   * Centre y = 768/2 + (-56) = 384 - 56 = 328 (0x43c00000 = 384 at 0x7a1dc4; 0xc2600000 = -56 at 0x7a1dd8,
//     added to both edges at 0x7a1df4/0x7a1df8). Top edge (v=0) y = 328 + halfExtent (w9, 0x7a1dfc), bottom edge
//     (v=1) y = 328 - halfExtent (w8, 0x7a1e00), each truncated to int16 (fcvtzs) and clamped to int16 range.
//   * Texcoords span the whole texture: (0,0)-(0x800,0x800), i.e. 0x800 = 1.0 (stores at 0x7a1db4..0x7a1da8).
//   * Colour = the four floats at 0x46ab40 = (1,1,1,1) white (cGlInterface::Colour, 0x7a1e44). No per-vertex alpha.
// The port reproduces this exactly: it reads the loaded texture's own dimensions back from GL (the engine cache
// owns the GL object) and places the SAME quad. The texcoord->vertex pairing is copied verbatim from the binary, so
// with the port's matching DXT decode (Assets_Texture) the art lands identically. Nothing is fabricated: only the
// user's own already-loaded bodyTexture_ is positioned.
//
// Observation: the x literals 144/1164, aspect = Width()/rawHeight, halfExtent = (1023/aspect)/2, centre 328,
// full-texture texcoords and white colour are all read from the binary. Assumption: the GL-queried width/height
// stand in for cTexture::Width()/+0x20 -- the bezel is a POT DXT texture, so the uploaded GL height equals the raw
// height and the GL width equals the POT width; if the engine ever uploaded it NPOT the aspect would differ. The
// game's 768x1024 PDA ortho is y-down (the clock sprite sits near the top at y=20), the same convention render()
// uses, so the binary's vertices are placed verbatim without a y-flip.
void Pda::drawBody() const {
    if (!bodyTexture_) return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, bodyTexture_);

    // cTexture::Width() (1<<widthLog2) and +0x20 (raw height), read back from the bound GL texture.
    GLint texW = 0, texH = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &texW);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &texH);
    if (texW <= 0 || texH <= 0) { glDisable(GL_TEXTURE_2D); return; }

    int aspect = texW / texH;               // sdiv w0, [+0x20]  (0x7a1d88), integer
    if (aspect < 1) aspect = 1;             // guard: the binary assumes Width() >= rawHeight (no divide-by-zero)
    const float halfExtent = (1023.f / static_cast<float>(aspect)) * 0.5f;   // (0x447f0000)/aspect, *0.5
    const float centreY = 384.f - 56.f;     // 768/2 (0x43c00000) + (-56, 0xc2600000) = 328
    const float yTop = static_cast<float>(static_cast<int>(centreY + halfExtent));   // w9: v=0 edge (fcvtzs)
    const float yBot = static_cast<float>(static_cast<int>(centreY - halfExtent));   // w8: v=1 edge (fcvtzs)
    const float x0 = 144.f;                 // 0x90
    const float x1 = 1164.f;                // 0x48c

    glColor4f(1.f, 1.f, 1.f, 1.f);          // floats at 0x46ab40 = (1,1,1,1)
    glBegin(GL_QUADS);
    // Verbatim from RenderPhoneSurround's four cGlVtx2dPT vertices (0x7a1d7c..0x7a1e38):
    glTexCoord2f(0.f, 0.f); glVertex2f(x0, yTop);   // V0: (x0, 328+half), uv (0,0)
    glTexCoord2f(1.f, 0.f); glVertex2f(x1, yTop);   // V1: (x1, 328+half), uv (1,0)
    glTexCoord2f(1.f, 1.f); glVertex2f(x1, yBot);   // V3: (x1, 328-half), uv (1,1)
    glTexCoord2f(0.f, 1.f); glVertex2f(x0, yBot);   // V2: (x0, 328-half), uv (0,1)
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

// ---------------------------------------------------------------------------
// HOME gallery (the PDA app launcher). See the header for the binary evidence: cTaskBar's sibling cShortcutBar
// (0x7c2b58) is a row of three 40x40 cButton slots at the container origin (slot rects {0,0,40,40} / {40,0,40,40} /
// {80,0,40,40}, read at 0x469870 / 0x467470 / 0x466570), whose icons come at run time from a cSpriteDataSet by
// index. Those per-app icon ids are a runtime assignment (not statically confirmable), so -- honouring the repo's
// draw-only-confirmed-art rule -- every tile here is a LABELLED tile (rect + app name), no guessed sprite id.
// ---------------------------------------------------------------------------
namespace {
// SDL scancodes (stable USB-HID usage ids SDL mirrors), matching the values game.cpp routes as `k`. Named here so
// pda.cpp needs no SDL header (verified against SDL2 SDL_scancode.h).
constexpr int kScA = 4, kScD = 7, kScS = 22, kScW = 26, kScReturn = 40, kScSpace = 44,
              kScRight = 79, kScLeft = 80, kScDown = 81, kScUp = 82, kScKpEnter = 88;

struct GalleryEntry { int appId; const char* name; };
// The shell's three hosted apps, in a fixed launch order (Stats first, as the PDA used to open straight into Stats).
constexpr GalleryEntry kGallery[] = {
    { kAppStats,    "STATS" },
    { kAppMap,      "MAP" },
    { kAppContacts, "CONTACTS" },
};
constexpr int kGalleryCount = static_cast<int>(sizeof(kGallery) / sizeof(kGallery[0]));
}  // namespace

// Open the PDA to the gallery: no hosted app, selection reset. Mirrors the role of landing on the task-bar home
// screen rather than straight into an app.
void Pda::openHome() {
    app_.reset();
    transition_.reset();
    targetApp_ = kAppNone;
    pendingApp_ = kAppNone;
    pendingSwitch_ = false;
    homeOpen_ = true;
    gallerySel_ = 0;
}

int Pda::gallerySelectedApp() const {
    int i = gallerySel_;
    if (i < 0 || i >= kGalleryCount) i = 0;
    return kGallery[i].appId;
}

// Route one key to the gallery. Left/Up/A/W = previous tile, Right/Down/D/S = next, Enter/KP-Enter/Space = open.
bool Pda::galleryInput(int scancode) {
    if (!homeOpen_)
        return false;
    const int n = kGalleryCount;
    switch (scancode) {
    case kScLeft: case kScA: case kScUp: case kScW:
        gallerySel_ = (gallerySel_ + n - 1) % n;
        return true;
    case kScRight: case kScD: case kScDown: case kScS:
        gallerySel_ = (gallerySel_ + 1) % n;
        return true;
    case kScReturn: case kScKpEnter: case kScSpace: {
        int appId = gallerySelectedApp();
        homeOpen_ = false;      // leave gallery mode; loadApp hosts the chosen app
        loadApp(appId);
        return true;
    }
    default:
        return false;
    }
}

// Back/Esc from an open app -> drop it and re-enter the gallery (the app's Leaving animation is asked for first, as
// cPda::LoadApp would, but the shell returns to gallery mode rather than loading another app).
void Pda::returnToHome() {
    if (app_)
        app_->transitionHeadingOff();
    app_.reset();
    transition_.reset();
    targetApp_ = kAppNone;
    pendingApp_ = kAppNone;
    pendingSwitch_ = false;
    homeOpen_ = true;
}

// Draw the gallery: a centred row of labelled app tiles with a visible highlight on the selected one. The real
// cShortcutBar row is 40x40 cells at the container origin; the shell presents a larger centred row here because it
// has no confirmed per-app icon atlas to reproduce the dock 1:1 (documented deviation). Only Hud_Rect / Hud_Text
// (the port's own 2D layer) are used -- no game art is drawn, nothing is fabricated.
void Pda::drawGallery(int width, int height) const {
    const int n = kGalleryCount;
    const float tileW = 180.f, tileH = 200.f, gap = 48.f;
    const float rowW = n * tileW + (n - 1) * gap;
    const float startX = (static_cast<float>(width) - rowW) * 0.5f;
    const float y = (static_cast<float>(height) - tileH) * 0.5f;

    // Heading above the row.
    const char* heading = "SELECT AN APP";
    const float headScale = 1.5f;
    const float headW = Hud_TextWidth(heading, headScale);
    Hud_Text((static_cast<float>(width) - headW) * 0.5f, y - 70.f, headScale, 0xBFD8E6FFu, heading);

    for (int i = 0; i < n; ++i) {
        const float x = startX + i * (tileW + gap);
        const bool sel = (i == gallerySel_);
        if (sel) {
            // Highlight frame behind the selected tile.
            Hud_Rect(x - 6.f, y - 6.f, tileW + 12.f, tileH + 12.f, 0x33CCFFFFu);
        }
        // Tile body (brighter when selected).
        Hud_Rect(x, y, tileW, tileH, sel ? 0x2E4A5AFFu : 0x1A232BFFu);
        // Inner "icon" placeholder plate (upper area of the tile) -- a plain plate, not borrowed art.
        const float pad = 24.f;
        Hud_Rect(x + pad, y + pad, tileW - 2 * pad, tileH - 2 * pad - 36.f, sel ? 0x0F161CFFu : 0x0C1116FFu);
        // App name, centred near the tile bottom.
        const char* name = kGallery[i].name;
        const float nameScale = 1.1f;
        const float nameW = Hud_TextWidth(name, nameScale);
        Hud_Text(x + (tileW - nameW) * 0.5f, y + tileH - 32.f, nameScale,
                 sel ? 0xFFFFFFFFu : 0xB0C2CEFFu, name);
    }
}

Pda::~Pda() {
    if (pdaTexture_) glDeleteTextures(1, &pdaTexture_);
    pdaTexture_ = 0;
}

namespace {
// cPda::RenderPhoneSurround / cPda::Init half-excess: the PDA runs in a 1024x768 (4:3) ortho; on a wider screen the
// visible area spills past 1024. The game computes width*768/height (the screen width measured in the 768-tall
// space), subtracts 1024 (rounding the <1024 case with 1023, matching 0x7a1c18/0x79fedc), and halves it. The result
// is how far the view extends to each side of the 1024-wide centre. 4:3 -> 0.
int surroundHalfExcess(int screenW, int screenH) {
    if (screenW <= 0 || screenH <= 0) return 0;
    int t = static_cast<int>((static_cast<long long>(screenW) * 768) / screenH);
    int d = t - 1024;
    if (d < 0) d = t - 1023;        // the binary's csel rounding for the (rare) sub-1024 case
    if (d < 0) d = 0;
    return d >> 1;                  // arithmetic halve (asr #1): the per-side extent
}
}  // namespace

// Draw the PDA frame and the hosted app's content area.
//
// cPda::Init (0x79fe24) creates the background sprite (+0x1668) from SS_AlwaysLoaded id 0x79 and shows it at
// cFixed<20,12> scale 0x2000 = 2.0 (cSprite::SetSpriteScale, 0x79ffbc). cPda::Render (0x7a130c) runs the PDA in a
// 1024x768 ortho (SetAspectCorrectOrthoMatrix with 0x44800000=1024, 0x44400000=768 at 0x7a13cc), draws this sprite
// behind the app, then calls cPda::RenderPhoneSurround (0x7a1b98) for the bezel. That bezel is a *separate*
// cTexture (+0x17dc), not part of SS_AlwaysLoaded; the port does not load it, so it is not drawn here (reported).
//
// Observation: the id (0x79), sheet (SS_AlwaysLoaded) and scale (2.0) are from the binary. Assumptions: the sprite
// is anchored at its own stored offX/offY handle with origin (0,0) in the 1024x768 space (cPda::Init adds it at a
// null position), and it is drawn at full brightness (white) -- cPda::Init writes colour 0xFF0000FF to the sprite's
// +0x74, but the channel order of that field is unconfirmed, so tinting the art by it is avoided; white shows the
// art unmodified, which is the intended look.
void Pda::render(int width, int height) const {
    if (frameSheet_ && frameTexture_) {
        drawAlwaysLoadedSprite(*frameSheet_, frameTexture_, frameTexW_, frameTexH_, 0x79,
                               0.f, 0.f, 2.f, 1.f, 0xFFFFFFFFu);
    } else {
        // Fallback when the sheet is not available: the dark PDA backdrop, so the app still reads on a solid field.
        Hud_Rect(0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0x0E1216FFu);
    }

    // The PDA device body/frame (loadBody): cPda::RenderPhoneSurround's per-theme bezel. drawBody() places it with
    // the binary's exact geometry -- x 144..1164, an aspect-fit vertical extent centred at y=328 -- BEHIND the app
    // content. Only drawn when loadBody() resolved the real game.pak texture; otherwise the current look is kept
    // (nothing fabricated).
    if (bodyTexture_)
        drawBody();

    if (app_)
        app_->onRender(width, height);
    else if (homeOpen_)
        drawGallery(width, height);   // HOME gallery: the labelled app tiles + selection, over the PDA body

    // cPdaApp::AddHeading (0x7a5d4c): every PDA app's title heading. The original adds a backing banner sprite from
    // the SS_PdaSprites set (cTaskBar's +0xfe0 / cPda +0xff8, AllocatePdaSpriteSet) plus the GXT title text on top.
    // When the app passes a null cSpriteData (0x7a5da8 csel), AddHeading falls back to the set's sprite-data entry at
    // byte offset 0xd0 -- i.e. SS_PdaSprites index 0xd0/16 = 13 (0x0d), within the 34-sprite atlas. The banner sprite
    // is added at (x=8, y=-0x20) with eScreenRestriction 3 and priority 0x10, default scale 1.0, white (AddSpriteImpl
    // 0x7a5dbc-0x7a5de8). Observation (from the binary): the default banner is SS_PdaSprites id 13 at (8,-32), and
    // cMapApp::Init (0x78f0c4 -> AddHeading with a null sprite, GXT 0x49), cContactsApp::Init (0x758d84, GXT 0x3e) and
    // cPauseStatsApp::Init (0x83fee0, GXT 0x43) all pass a null sprite, so all three hosted apps share this banner.
    // Assumption: for a left-anchored element the 1024-wide box origin (0) is used (no widescreen half-excess, unlike
    // the right-anchored clock), and eScreenRestriction 3 keeps it at the box left; the per-app title TEXT on top is
    // the port's own text layer (the game's GXT/sprite-font pipeline is out of this shell), so only the banner art is
    // drawn here. The home/back/email/exit task-bar buttons are NOT in SS_PdaSprites: cTaskBar::Init (0x7c7dfc,
    // 0x7c7e54) sets their backing sprites from the SS_AlwaysLoaded set (+0xfd8) -- home id 0x4d (off 0x4d0), back id
    // 0x60 backing / 0x61 highlight (off 0x600/0x610) -- so they belong to the frame sheet, not this atlas, and are
    // deliberately not drawn from the PDA atlas here (drawing them from it would show the wrong art).
    if (themeLoaded() && app_ && app_->id() != kAppNone && app_->id() != kAppHud) {
        drawPdaSprite(0x0d, 8.f, -32.f, 1.f, 1.f, 0xFFFFFFFFu);
    }

    // cTaskBar home / back task-bar buttons (cTaskBar::Init 0x7c7d6c; backing sprites set from the SS_AlwaysLoaded
    // set -- cTaskBar +0xfd8, GetSpriteDataAllwaysLoaded -- NOT the SS_PdaSprites atlas, so they are drawn here from
    // the already-loaded frame sheet by id). Nothing is recreated: the user's own loaded pixels, selected by id.
    //   HOME = cTaskBar +0x788, SetBackingSprite from always-loaded data +0x4d0 => sheet id 0x4d (0x4d0/16).
    //   BACK = cTaskBar +0xb58, backing data +0x600 => id 0x60; highlight data +0x610 => id 0x61 (hover/press only).
    //
    // REAL on-screen position & scale (observation -- the earlier (0,0) placement was wrong). The buttons are
    // cButton children of the task bar, and cTaskBar::Process (0x7c85b4) lays them out every frame. Their absolute
    // 1024x768 position is the task bar window's PositionScreenSpace plus the per-button offset. The task bar sits at
    // the PDA frame origin (0,0): its sibling shortcut container is constructed at x=0,y=0 (cTaskBar::cTaskBar,
    // ctor call 0x7c799c) and the task bar / cPdaApp window itself is never repositioned, so PositionScreenSpace
    // resolves to (0,0) in 4:3 -- i.e. the bar runs along the TOP edge. cTaskBar::Process then places the visible
    // backing sprites (all literals):
    //   BACK: window SetPosition(0,0) dims 55x38 (0x7c88c8/0x7c88d8); backing sprite SetSpriteScale(0x1000 = 1.0)
    //         and SetSpritePos(0x12 = 18, -6) (0x7c88ec/0x7c8908). The sprite handle lands at (18,-6), scale 1.0.
    //   HOME: window dims 80x80 on a phone (IsAPhone path, 0x7c87b8); backing sprite scale 2.0 (0x7c87f8); centered
    //         in the window via SetSpritePos(center - scaledSprite/2), center = (width/2, height/2) = (40,40)
    //         (0x7c878c-0x7c889c). drawAlwaysLoadedSprite anchors the sprite's own offX/offY handle at the passed
    //         (x,y) exactly as cSprite::CalculateVerts does, so passing the game's SetSpritePos value reproduces the
    //         placement: HOME x,y = 40 - (nativeW*scale)/2, 40 - (nativeH*scale)/2 (nativeW = d.w+1 in this sheet).
    // Visibility (cTaskBar::ShowHomeButton 0x7c91b0 / ShowBackButton 0x7c97ac): kept from the prior pass -- draw home
    // when a non-HUD app is open; the exact per-app "can go back" gate for BACK is still approximated the same way.
    // Assumptions: IsAPhone==true (the libGame.so handheld path; a tablet/desktop uses scale 1.25 for home), and the
    // widescreen half-excess is 0 (4:3, so PositionScreenSpace == (0,0)). The ids, window rects, scales and
    // SetSpritePos offsets are observation. The 0x61 highlight is hover/press-only and, with no input wired, not drawn.
    if (frameSheet_ && frameTexture_ && app_ && app_->id() != kAppNone && app_->id() != kAppHud) {
        const auto& sprites = frameSheet_->sprites();
        const float homeScale = 2.f;   // cTaskBar::Process IsAPhone path (0x7c87f8)
        if (0x4d < (int)sprites.size()) {
            const SpriteDef& hd = sprites[0x4d];
            const float hx = 40.f - (hd.w + 1.f) * homeScale * 0.5f;   // center (40,40) - scaledSprite/2
            const float hy = 40.f - (hd.h + 1.f) * homeScale * 0.5f;
            drawAlwaysLoadedSprite(*frameSheet_, frameTexture_, frameTexW_, frameTexH_, 0x4d,
                                   hx, hy, homeScale, 1.f, 0xFFFFFFFFu);   // HOME (id 0x4d), centered in its 80x80 window
        }
        drawAlwaysLoadedSprite(*frameSheet_, frameTexture_, frameTexW_, frameTexH_, 0x60,
                               18.f, -6.f, 1.f, 1.f, 0xFFFFFFFFu);   // BACK (id 0x60): SetSpritePos(18,-6), scale 1.0
    }

    // cPda::RenderPhoneSurround (0x7a1b98): the widescreen letterbox. The original unbinds the texture and draws one
    // black quad (cGlVtx2dPC) spanning the whole widened width [-he, 1024+he] x [0, 768] with RGB 0 and alpha 0x80
    // (128); alpha is forced to 0xFF (255) when OS_SystemForm()==1 (0x7a1c6c-0x7a1c78). It then covers the centre
    // (x 0..1024) with the themed phone bezel, so only the two strips outside the 4:3 centre show as black bars.
    // The port draws only those side strips (the centre holds our background art and has no bezel to cover it); with
    // no screen metrics set, he == 0 and nothing is drawn. Colour/alpha and geometry are from the binary;
    // observation: RGB 0, alpha 128, per-side extent = surroundHalfExcess; assumption: the common alpha is 128 (the
    // OS_SystemForm()==1 -> 255 case is not queryable here, so the handheld value is used).
    const int he = surroundHalfExcess(screenW_, screenH_);
    if (he > 0) {
        Hud_Rect(static_cast<float>(-he), 0.f, static_cast<float>(he), static_cast<float>(height), 0x00000080u);
        Hud_Rect(static_cast<float>(width), 0.f, static_cast<float>(he), static_cast<float>(height), 0x00000080u);
    }

    // cPda::ReprintTodSprite (0x7a0dcc): the time-of-day clock, a text sprite (+0x1700, created in cPda::Init at
    // x = halfExcess + 0x3b8 = 952, y = 0x14 = 20, font scale 1.25 via SetPhoneTextScale). Its text is
    // cTimeCycle::TimeStr() -- a 5-char buffer (the "00000" placeholder at 0x4703dd), i.e. "HH:MM". The sprite is
    // the port's own text (the game's sprite manager / font theme is out of this shell), so it is drawn here with
    // Hud_Text using the clock captured in process(). Observation: position/scale/5-char buffer from the binary;
    // assumption: the format is 24h "HH:MM" with a colon, and white (the sprite colour field is not clearly set).
    // Faithful gate (ReprintTodSprite 0x7a0e24): the clock shows only while the HUD home app (+0x1660==0x10) is up;
    // the port's shell opens an app directly and has no HUD home app, so the clock is drawn whenever the PDA is
    // enabled, as a persistent top-chrome element (a documented deviation so the chrome is visible).
    if (enabled_) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", clockHour_, clockMinute_);
        const float clockX = static_cast<float>(952 + he);
        Hud_Text(clockX, 20.f, 1.25f, 0xFFFFFFFFu, buf);
    }

    if (loadingShown_ && loadingAlpha_ > 0.f) {
        uint32_t a = static_cast<uint32_t>(loadingAlpha_ * 255.f) & 0xFF;
        drawScreenRect(a << 24, 0, 0, width, height);
    }
    if (transition_ && transition_->isActive()) {
        float c = transition_->cover();
        if (c > 0.f) {
            uint32_t a = static_cast<uint32_t>(c * 255.f) & 0xFF;
            drawScreenRect(a << 24, 0, 0, width, height);
        }
    }
}

}  // namespace pda
