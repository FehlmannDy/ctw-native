// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Gui::cPda: the PDA shell. It opens and closes the PDA, draws the frame (clock, loading overlay, screen
// rectangle), and hosts one app at a time through the app lifecycle + load/transition state machine. The hosted
// content (the Stats app, the map, the radio, etc.) lives in separate files and plugs in through PdaApp and the
// app factory; this shell owns everything around the app's content area.
//
// Ported routines and their evidence (observation unless marked assumption):
//   cPda::Init             0x79fe24  creates the clock text sprite (+0x1700) and background (+0x1668), singleton.
//   cPda::Enable           0x7a3b5c  +0x1735 = enable & WorldImpl::HaveShownStings().
//   cPda::Process          0x7a21e8  per-frame: step transition, route input when the app is interactive, run the
//                                     app's state machine, defered loading, clock flash, loading-overlay fade.
//   cPda::ProcessInput     0x7a25f8  decrement the open input-delay (+0x1730), then route touch to app / taskbar.
//   cPda::LoadApp          0x7a2db8  pick a transition for the target app id (+0x1660), arm the pending switch.
//   cPda::ProcessLoadApp   0x7a3b94  at the transition midpoint, SwitchApp unless an app is still running.
//   cPda::IsAppRunning     0x7a21bc  no app, or the app's state (+0x138) == 2 (Running).
//   cPda::IsTransitioning  0x7a25a4  a live transition, or the app not yet settled into Running.
//   cPda::ProcessDeferedAppLoading 0x7a27a4  a queued app id (+0x176c, -1 none) fires once the app is settled.
//   cPda::FlashClock       0x7a0f90  the clock's minute-tick flash flag (+0x1768), gated to HUD/map apps.
//   cPda::DrawScreenRect   0x7a4e94  a filled quad (white, alpha = colour>>24) used by the fade transitions.
//   cPda::DisplayLoading   0x7a5020  show/hide the loading spinner (+0x17d0) with its fade state (+0x17c8).
//
// The background art (+0x1668, SS_AlwaysLoaded id 0x79) is now drawn in render() via setFrameSprites().
//   cPda::RenderPhoneSurround 0x7a1b98  the widescreen side letterbox bars (black, alpha 128) are drawn in render()
//                                        from the real screen size (setScreenMetrics); the themed bezel (+0x17dc)
//                                        is still blocked (see below).
//   cPda::ReprintTodSprite    0x7a0dcc  the time-of-day clock (+0x1700, "HH:MM" from cTimeCycle::TimeStr) is drawn
//                                        in render() from the clock captured in process().
// Still deferred (asset / cross-system, out of this shell's scope): the clock's weak-pointer callback scheduler and
// the minute-tick flash/SFX inside FlashClock, the day/loading sprite art. The phone bezel is a cTexture at +0x17dc
// that is NOT in SS_AlwaysLoaded: it is loaded by the active PDA theme object (+0x17c0, one of
// cThemeBadger/cThemeSprunk/cThemeTwat/cThemeLaptop built in cPda::SwitchPdaTheme 0x7a0bc4; the theme's apply vtable
// slot stores +0x17dc). It is a DXT texture inside game.pak (not a loose PNG); loadBody() now loads it through the
// engine's resource/texture cache at runtime and render() draws it as the PDA body (see loadBody). Also out of
// scope: the help-tip manager (+0x15a0), the screen saver (+0x1678), the email / vigilante managers, and GXT text.
#pragma once
#include "gfx/spriteset.h"   // owned PDA sprite sheet (SS_PdaSprites) loaded by loadTheme()
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pda {

// A handful of ePdaApp ids seen in the binary; the full enum is the orchestrator's. 0 is "no app".
enum : int {
    kAppNone = 0x00,
    kAppHud  = 0x10,   // cPda::Process / FlashClock gate on this and the map
    kAppMap  = 0x4a,
    // Appended: the shell's other two hosted apps, used by the HOME gallery launcher (openHome). Ids observed from
    // the binary (cPauseStatsApp 0x30 via the orchestrator; cContactsApp 0x4b, the map's neighbour).
    kAppStats    = 0x30,
    kAppContacts = 0x4b,
};

// eResetOption passed through LoadApp (+0x1770). The shell only stores it; apps interpret it.
enum class ResetOption : int { None = 0, Reset = 1, Soft = 2 };

// cPdaApp::ProcessAllStates drives this lifecycle; 2 == Running is the one other routines test (+0x138).
enum class AppState : int {
    Loading     = 0,   // waiting on the app's resources
    Appearing   = 1,   // playing its intro; input already routed here
    Running     = 2,   // settled; IsAppRunning true
    Leaving     = 3,   // TransitionHeadingOff asked it to go
};

// A touch, mirroring the port's sTouch usage (window pixels; held while down).
struct Touch { bool down = false; bool pressed = false; float x = 0, y = 0; };

// The content hosted inside the PDA frame. The base reproduces cPdaApp's shared state machine
// (ProcessAllStates); a concrete app overrides the hooks to supply its content. The shell owns the frame and
// calls these; the app draws only its content area.
class PdaApp {
public:
    explicit PdaApp(int appId) : id_(appId) {}
    virtual ~PdaApp() = default;

    int id() const { return id_; }                 // ePdaApp (+0x144)
    AppState state() const { return state_; }       // +0x138
    bool busy() const { return busy_; }             // +0x168: not yet settled into Running
    bool finished() const { return finished_; }     // its Leaving animation is done; the shell may drop it

    // cPdaApp::ProcessAllStates: advance Loading -> Appearing -> Running, and Leaving -> finished.
    void processAllStates();
    // cPdaApp::TransitionHeadingOff: begin leaving.
    void transitionHeadingOff();

    // Hooks (defaults make a trivial app that loads and appears instantly):
    virtual bool onLoad() { return true; }     // true when resources are ready (Loading -> Appearing)
    virtual bool onAppear() { return true; }   // true when the intro is done (Appearing -> Running)
    virtual void onProcess() {}                // one frame while Running
    virtual bool onLeave() { return true; }    // true when the outro is done (Leaving -> finished)
    virtual void onInput(const Touch&) {}      // cPdaApp vtable +0xe0, while Appearing/Running
    virtual void onRender(int /*w*/, int /*h*/) const {}  // draw the content area

private:
    int id_ = kAppNone;
    AppState state_ = AppState::Loading;
    bool busy_ = true;
    bool finished_ = false;
};

// The screen-level animation between apps (cPda +0x1718). The concrete flavours in the binary are
// cInstantNoFadeTransition, cPdaTransWhiteFade and cPdaTransLoadingAnimation; this reproduces their roles: it
// plays for a span, reveals the new app at its midpoint (where SwitchApp runs), then settles.
class PdaTransition {
public:
    enum class Kind { Instant, WhiteFade, LoadingAnimation };
    explicit PdaTransition(Kind kind, int frames = 10) : kind_(kind), total_(frames) {}

    void process() { if (frame_ < total_) ++frame_; }     // step one frame
    bool isActive() const { return frame_ < total_; }      // vtable +0x20
    bool atMidpoint() const { return frame_ == total_ / 2; } // where the app is swapped under cover
    float cover() const;                                   // 0..1 fade used by render
    Kind kind() const { return kind_; }

private:
    Kind kind_;
    int frame_ = 0, total_ = 10;
};

class Pda {
public:
    Pda() = default;
    ~Pda();                        // releases the GL texture owned by loadTheme()
    Pda(const Pda&) = delete;
    Pda& operator=(const Pda&) = delete;

    // cPda::Init: the shell is ready; the orchestrator must set appFactory before loading an app.
    void init();
    // cPda::Enable: only takes when the stings have been shown (haveShownStings).
    void enable(bool on, bool haveShownStings);
    bool enabled() const { return enabled_; }

    // The orchestrator supplies this: build the app object for an ePdaApp id (or null for a stub).
    std::function<std::unique_ptr<PdaApp>(int appId, ResetOption reset)> appFactory;

    // cPda::LoadApp: switch to appId with a transition. reset/forward are stored and passed to the factory.
    void loadApp(int appId, ResetOption reset = ResetOption::None);
    // Queue a load that fires once the current app has settled (cPda::ProcessDeferedAppLoading via +0x176c).
    void deferLoadApp(int appId, ResetOption reset = ResetOption::None);

    // cPda::Process: one 30 Hz frame. minute/hour are the game clock (WorldImpl's time), used by FlashClock.
    void process(int minute, int hour);
    // cPda::ProcessInput: route a touch for this frame (after the open input-delay has run down).
    void processInput(const Touch& touch);

    // Draw the frame and the hosted app's content area.
    void render(int width, int height) const;

    // Hand the shell the always-loaded sprite sheet so render() can draw the real PDA frame art. cPda::Init
    // (0x79fe24) builds the background sprite from SS_AlwaysLoaded id 0x79 (cTaskBar::GetSpriteDataAllwaysLoaded,
    // which indexes the sheet directly by id) and shows it at scale 2.0 behind the app. The orchestrator passes
    // the sheet and texture from GameplayHud::alwaysLoaded() / alwaysLoadedTexture() and the PNG's pixel size
    // from alwaysLoadedWidth()/Height(). With a null sheet, render() falls back to the dark backdrop.
    void setFrameSprites(const SpriteSet* sheet, unsigned texture, int texWidth, int texHeight);

    // cPda::RenderPhoneSurround (0x7a1b98): the widescreen letterbox. The original reads the real screen size and
    // computes a half-excess (width*768/height, minus 1024, halved), fills one black quad across the whole widened
    // width, then covers the centre with the themed phone bezel. Give the shell the real screen size so render()
    // can draw the side letterbox bars at the faithful geometry/colour; with no size set (0,0 -> 4:3) no bars are
    // drawn. Append-only; the orchestrator may pass OS_ScreenGetWidth()/Height() (or leave it unset for 4:3).
    void setScreenMetrics(int screenWidth, int screenHeight);

    // Load the PDA sprite sheet the same way GameplayHud::init loads SS_AlwaysLoaded: its definitions come from
    // "SS_PdaSprites.bin" (the name the binary passes to cSpriteDataSet in cTaskBar::AllocatePdaSpriteSet,
    // 0x007ca04c, string at 0x472a57) read through GameFs (ROM.WAD), and the pixels from the loose
    // "ss_pdasprites.png" (a 2048x512 texture, i.e. a 1024x256 layout at the SS_ 2x PNG scale). The parsed
    // SpriteSet and the GL texture are owned by the shell. Returns false (and leaves the shell unthemed) if any
    // part is missing, so the orchestrator may call it once in Game::init after the HUD/data dir is ready.
    //
    // Note from the binary (reported, not an omission): SS_PdaSprites is the task-bar / per-app CHROME atlas
    // (34 sprites: app-title banners and the home/back buttons drawn by cTaskBar::Render), NOT the phone device
    // frame. cPda::Init's backdrop sprite (+0x1668) is SS_AlwaysLoaded id 0x79 (already drawn in render via
    // setFrameSprites); the phone bezel is cPda::RenderPhoneSurround's per-theme cTexture (+0x17dc), loaded
    // through cResourceManager/cTextureManager (cThemeBadger::Init -> cPdaTheme::SetupBG, resource index 0x59),
    // which is a different pipeline from the sprite-sheet mechanism and is not shipped as a loose PNG. So this
    // loader makes the real PDA chrome atlas available (for the hosted apps / task bar); it does not fabricate a
    // device frame that the data does not contain.
    bool loadTheme(const std::string& dataDir);
    bool themeLoaded() const { return pdaTexture_ != 0 && !pdaSheet_.sprites().empty(); }
    // The loaded PDA chrome atlas, for the hosted apps / task bar to draw their real sprites by id.
    const SpriteSet& pdaSheet() const { return pdaSheet_; }
    unsigned pdaTexture() const { return pdaTexture_; }
    int pdaSheetWidth() const { return pdaTexW_; }
    int pdaSheetHeight() const { return pdaTexH_; }
    // Draw one SS_PdaSprites sprite by index, with the SS_ half-res PNG texel convention (same path the HUD uses
    // for SS_AlwaysLoaded). No-op until loadTheme() has succeeded.
    void drawPdaSprite(int index, float x, float y, float scale, float alpha = 1.f,
                       uint32_t tint = 0xFFFFFFFFu) const;

    // Load the PDA device body/frame -- the phone bezel cPda::RenderPhoneSurround (0x7a1b98) draws from its
    // per-theme cTexture (+0x17dc). The orchestrator reported this art is a DXT-compressed texture inside game.pak
    // (NOT a loose PNG), so it is loaded through the engine's resource/texture loader at runtime, exactly as the
    // game does -- no PNG, no extraction, no hand-drawing. Faithful chain:
    //   cThemeBadger::Init (0x7ca7d8) -> cPdaTheme::SetupBG(0x59) (0x7ab518): cResourceManager::Load the active
    //   theme's u16 texture-id TABLE resource, read table[0x59] = the bezel texture id, then
    //   cTextureManager::Load(texId). cTextureManager::Load (0x6d9ac4) uses texId directly as the game.pak
    //   resource id, which is what the port's Assets_Texture() consumes.
    // So the port mirrors it: read the table resource with Assets_Pak().read(themeTableResId), take its little-endian
    // u16 at bezelIndex (= texId), and hand texId to Assets_Texture() (which resolves dxt.bin / game.pak and decodes
    // DXT, like cResourceManager::Load + cTexture::TexImage). The returned GL texture is owned by the engine's
    // texture cache, NOT by the shell. Returns false -- leaving the body unset so render() keeps the current look --
    // if the table or texture is unavailable, so nothing is fabricated when the data is absent.
    //
    // themeTableResId is the game.pak resource id of the ACTIVE theme's texture-id table. In libGame.so this is a
    // RUNTIME value: cPdaTheme::SetupBG reads it from cResourceManager's directory object (*(0xdce000+0xa88)+0x34),
    // which is NOT a static constant (0xdcea88 is zero-init .bss, set at load time) and is not reachable through the
    // current Assets API. So the orchestrator must supply it: it is a single u16 read from the user's OWN game.pak
    // directory (the Porter can dump it), nothing fabricated here. bezelIndex defaults to the binary's 0x59.
    // Requires Assets_Open(dataDir) to have run first (Game::init already opens game.pak/dxt.bin).
    bool loadBody(const std::string& dataDir, int themeTableResId, int bezelIndex = 0x59);
    bool bodyLoaded() const { return bodyTexture_ != 0; }

    bool isAppRunning() const;        // cPda::IsAppRunning
    bool isTransitioning() const;     // cPda::IsTransitioning
    PdaApp* app() const { return app_.get(); }
    int targetApp() const { return targetApp_; }      // +0x1660
    bool clockFlash() const { return clockFlash_; }   // +0x1768

    // cPda::DrawScreenRect: a filled rectangle. The original forces the quad white and takes only the alpha byte
    // of colour (colour>>24); the port keeps the whole rgba and documents that.
    static void drawScreenRect(uint32_t colour, int x0, int y0, int x1, int y1);
    // cPda::DisplayLoading: show or hide the loading overlay (fades in to half alpha, or out and removed).
    void displayLoading(bool show);

    // ----- HOME gallery (the PDA app launcher) --------------------------------------------------------------
    // The PDA opens to a HOME gallery: a row of selectable app-icon tiles (Stats / Map / Contacts). Choosing a
    // tile loads that app; Back/Esc from an app returns here. This reproduces the ROLE of cTaskBar's app-shortcut
    // container -- the sibling container built at (0,0) in cTaskBar::cTaskBar (0x7c784c) holds a cShortcutBar
    // (0x7c2b58) whose three cButton slots are laid out as a horizontal row of 40x40 cells at the container origin.
    // The slot rects are read from the binary (cButton::cButton calls in the ctor): slot0 {x=0,y=0,w=40,h=40}
    // (0x469870), slot1 {x=40,y=0,w=40,h=40} (0x467470), slot2 {x=80,y=0,w=40,h=40} (0x466570). Each slot's icon is
    // supplied at run time from a cSpriteDataSet, indexed by cShortcutBar +0x530/+0x534/+0x538 and drawn by
    // cShortcutBar::RefreshSprites (0x7c2de8); OnWindowDropped (0x7c3050) hit-tests those three rects to open the
    // dropped app. Those per-slot icon indices are a RUNTIME assignment (the ctor sets them to -1; the owner that
    // knows the ePdaApp->icon mapping fills them), so they are NOT statically confirmable to Stats/Map/Contacts.
    // Per the repo's draw-only-confirmed-art rule, the gallery therefore draws each app as a LABELLED tile (a rect
    // plus the app name via Hud_Text) and never guesses a sprite id that could show the wrong art. (Observation:
    // the three slot rects, the 40x40 cells and the row origin; assumption: the gallery's own larger centred
    // presentation, since the shell has no confirmed per-app icon atlas for the home grid to reproduce 1:1.)

    // Open the PDA to the gallery (no hosted app yet, selection reset). The orchestrator calls this to open the PDA
    // instead of loadApp(kAppStats). Call enable(...) first, as before.
    void openHome();
    bool homeOpen() const { return homeOpen_; }

    // Route one key (an SDL scancode, exactly as game.cpp forwards `k`) to the gallery: Left/Up/A/W select the
    // previous tile, Right/Down/D/S the next, and Enter/KP-Enter/Space open the selected app (loadApp + leave
    // gallery mode). Returns true when the key was consumed; the orchestrator keeps its own handling (e.g. Esc
    // closes the PDA) for keys this leaves unhandled (returns false).
    bool galleryInput(int scancode);

    // Back/Esc from an open app: drop the app and return to the gallery (re-enter gallery mode). The orchestrator
    // calls this when an app is open and the player presses Back/Esc, instead of closing the PDA.
    void returnToHome();

    // The ePdaApp id of the currently highlighted gallery tile (for the orchestrator / tests).
    int gallerySelectedApp() const;

private:
    void processTransitions();             // step the transition, run the midpoint switch
    void processLoadApp(bool appStillRunning, bool reset);  // cPda::ProcessLoadApp
    void switchApp(bool reset);            // cPda::SwitchApp: drop the old app, build the pending one
    void processDeferedAppLoading();       // cPda::ProcessDeferedAppLoading
    bool flashClock(int minute, int hour); // cPda::FlashClock
    void updateLoadingOverlay();           // the +0x17c8 fade stepped each frame in Process
    void drawBody() const;                 // draw the loadBody() bezel over the 1024x768 PDA centre (RenderPhoneSurround)
    void drawGallery(int width, int height) const;  // draw the HOME gallery's labelled app tiles + selection

    std::unique_ptr<PdaApp> app_;          // +0x08
    std::unique_ptr<PdaTransition> transition_;  // +0x1718
    int targetApp_ = kAppNone;             // +0x1660: the app the pending/last load resolves to
    int pendingApp_ = kAppNone;            // the app switchApp will build at the transition midpoint
    ResetOption pendingReset_ = ResetOption::None;
    bool pendingSwitch_ = false;           // +0x1736: a load armed a transition; switchApp still owed
    bool enabled_ = false;                 // +0x1735
    int inputDelay_ = 0;                   // +0x1730: frames of input swallowed after opening

    int deferredApp_ = -1;                 // +0x176c: queued load (-1 none)
    ResetOption deferredReset_ = ResetOption::None;  // +0x1770

    // FlashClock state
    int flashMinute_ = -1;                 // +0x174c: the minute last shown
    bool clockFlash_ = false;              // +0x1768

    // DisplayLoading overlay: 0 idle, 1 fading in (to 0.5), 2 fading out (to 0, then removed)
    int loadingState_ = 0;                 // +0x17c8
    float loadingAlpha_ = 0.f;             // the overlay sprite's alpha
    bool loadingShown_ = false;            // the +0x17d0 sprite exists

    // The always-loaded sheet for the PDA frame art (set by the orchestrator; null -> dark backdrop fallback).
    const SpriteSet* frameSheet_ = nullptr;   // +0x1668 background is SS_AlwaysLoaded id 0x79
    unsigned frameTexture_ = 0;
    int frameTexW_ = 0, frameTexH_ = 0;

    // The PDA chrome atlas owned by loadTheme(): SS_PdaSprites defs + ss_pdasprites.png as a GL texture.
    SpriteSet pdaSheet_;                       // cTaskBar's SS_PdaSprites (AllocatePdaSpriteSet, 0x7ca04c)
    unsigned pdaTexture_ = 0;
    int pdaTexW_ = 0, pdaTexH_ = 0;

    // The PDA device body/frame bezel (loadBody): a game.pak DXT texture resolved by resource id through the
    // engine's Assets_Texture cache. NON-OWNED (the engine owns the GL texture, so the destructor must not delete
    // it); 0 when no body is loaded, in which case render() keeps the current background look.
    unsigned bodyTexture_ = 0;

    // Real screen size for the RenderPhoneSurround half-excess (0,0 -> 4:3, no side bars). Set by the orchestrator.
    int screenW_ = 0, screenH_ = 0;
    // The game clock captured in process(), drawn by the clock text sprite (+0x1700 / cPda::ReprintTodSprite).
    int clockHour_ = 0, clockMinute_ = 0;

    // HOME gallery (the app launcher). homeOpen_ puts the shell in gallery mode (no hosted app); gallerySel_ is the
    // highlighted tile index into the fixed app list (see pda.cpp). Entries are drawn as labelled tiles (openHome()).
    bool homeOpen_ = false;
    int gallerySel_ = 0;

    friend struct PdaTestAccess;
};

}  // namespace pda
