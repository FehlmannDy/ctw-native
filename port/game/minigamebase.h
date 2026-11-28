// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// The shared minigame framework that every PDA minigame (SinkingCar, HotWiring, SafeCracking, BombDisposal,
// EmptyTheBoot, DrugVanSlash, ...) sits on. In libGame.so this is spread over three cooperating pieces, all
// reproduced here as self-contained C++ with the engine edges injected (no world/engine includes):
//
//   cGenericPlayerMiniGameTask  the player-side task that owns a running minigame: it carries the player-status
//                               requirement (+0x2c), the pass/fail/quit result (+0x5c), and the completion
//                               callback (+0x30/+0x38/+0x40); IsValid gates on foot vs in a vehicle; OnEndApp
//                               hands the PDA back to the HUD app; Abort/OnFinished end it.
//   MiniGameTimerBar            the attempt timer shared by the timed minigames: Initialise sets the frame limit,
//                               Update ticks one 30 Hz frame and reports expiry.
//   cMiniGameResultTextHelper   the win/lose "result" text outro: a 4-phase fade-in / show / pause / done state
//                               machine driving the result sprites' alpha.
//
// PDAImpl's WasMiniGamePassed / WasMiniGameFailed / WasMiniGameQuit / IsAnyMinigameRunning read the result field
// and the running ePdaApp id; they are reproduced as free queries over a Minigame and its host.
//
// Evidence is cited by address and offset; everything is observation from libGame.so unless marked ASSUMPTION.
// The concrete minigame apps and the PDA/script wiring that starts them live in other files and are reported, not
// included, here.
#pragma once
#include <cstdint>
#include <functional>

namespace minigame {

// eMiniGamePlayerStatus, the second ctor argument of cGenericPlayerMiniGameTask (0x0070d4c4, stored at +0x2c) and
// the selector tested by cGenericPlayerMiniGameTask::IsValid (0x0070d8a4):
//   status 0        -> always valid (IsValid returns 1 immediately)
//   status 1 OnFoot -> valid only when the player's ped has NO vehicle (cPed::Vehicle() == null)
//   status 2 InVeh  -> valid only when the player's ped HAS a vehicle
enum class PlayerStatus : int { Any = 0, OnFoot = 1, InVehicle = 2 };

// The pass/fail/quit result stored in the task at +0x5c. The ctor initialises it to 1 (Passed) (0x0070d528:
// mov w9,#1 / str w9,[this,#0x5c]). The mapping is read back out by PDAImpl:
//   WasMiniGamePassed (0x008b2a34): +0x5c == 1
//   WasMiniGameFailed (0x008b2aa4): +0x5c == 0
//   WasMiniGameQuit   (0x008b29c4): +0x5c == 2
enum class Result : int { Failed = 0, Passed = 1, Quit = 2 };

// A minigame occupies an ePdaApp id at or above 0x34. PDAImpl::IsAnyMinigameRunning (0x008b2c0c) returns
// RunningAppType() > 0x33; the Was* queries all early-out when RunningAppType() < 0x34. The reference minigame
// cMiniGameDummy::Init (0x00942f4c) narrows the live dummy range to [0x3c, 0x58] (RunningAppType - 0x3c <= 0x1c).
inline constexpr int kFirstMinigameApp = 0x34;   // cmp #0x34 / b.lo in the Was* queries
inline constexpr int kHudApp = 0x10;             // cGenericPlayerMiniGameTask::OnEndApp loads ePdaApp 0x10
constexpr bool isMinigameApp(int ePdaApp) { return ePdaApp >= kFirstMinigameApp; }

// MiniGameTimerBar (0x00804ffc ctor, 0x00805020 Initialise, 0x00805048 Update). The data model only -- the
// MetaSprite pointers and Render/RenderRect/SendVertices (GL) are reported, not ported. Elapsed (+0x20) and the
// limit (+0x22) are u16 frame counts at 30 Hz.
class TimerBar {
public:
    // MiniGameTimerBar::Initialise(bar, frame, limit): stores the limit (+0x22) and zeroes the elapsed count
    // (+0x20). The two MetaSprite args are render data and are omitted here.
    void initialise(uint16_t limitFrames) { limit_ = limitFrames; elapsed_ = 0; }
    // MiniGameTimerBar::Update: ++elapsed, then return (limit <= elapsed) -- true once the bar has run out.
    // (0x0080505c cset w0, ls on cmp limit, elapsed+1.)
    bool update();
    uint16_t elapsed() const { return elapsed_; }
    uint16_t limit() const { return limit_; }
    bool expired() const { return limit_ != 0 && elapsed_ >= limit_; }

private:
    uint16_t elapsed_ = 0;   // +0x20
    uint16_t limit_ = 0;     // +0x22
};

// cMiniGameResultTextHelper (0x0080fce8 ctor, 0x00810040 Begin, 0x00810038 SetPauseTime, 0x0081004c Update).
// The outro that fades the win/lose text in, shows it, holds for a pause, then signals done. The port keeps the
// two alpha ramps and the phase machine; the actual cSpriteWindow alpha writes are delivered through onAlpha /
// onShow hooks so the type stays self-contained (the original calls Gfx2d::cSprite::SetAlphaValue/ShowSprite).
class ResultText {
public:
    // The phase field (+0x08). 0 is the idle/unstarted phase; Begin moves it to 1.
    enum class Phase : int { Idle = 0, FadeIn = 1, Show = 2, Pause = 3, Done = 4 };

    // ctor constants from 0x0080fce8: black-poly ramp {cur 0, target 0.75, step 0.07} at +0xc/+0x10/+0x14
    // (dword_4692d0), text-alpha ramp {cur 0, target 1.0, step 0.1} at +0x18/+0x1c/+0x20 (dword_47d218), and the
    // default pause time 20 frames at +0x48 (dword_47e548 low word).
    ResultText() = default;

    void begin() { phase_ = Phase::FadeIn; }                 // Begin (0x00810040): +0x8 = 1
    void setPauseTime(int frames) { pauseTime_ = frames; }    // SetPauseTime (0x00810038): +0x48 = arg
    void turnTextOnOff(bool on) { visible_ = on; if (onShow) onShow(on); }   // TurnTextOnOff (0x0080fec0)

    // Update (0x0081004c): advance one frame; returns true only in the Done phase (w20 == 1 at +0x8 == 4).
    bool update();

    Phase phase() const { return phase_; }
    float textAlpha() const { return textAlpha_; }    // +0x18, the alpha pushed to the result sprites
    float blackAlpha() const { return blackAlpha_; }  // +0xc, the RenderBlackPoly backing alpha
    bool visible() const { return visible_; }

    std::function<void(float alpha)> onAlpha;   // SetAlphaValue(+0x18) on both result sprite windows
    std::function<void(bool on)> onShow;        // ShowSprite on both result sprite windows

private:
    Phase phase_ = Phase::Idle;   // +0x08
    float blackAlpha_ = 0.f, blackTarget_ = 0.75f, blackStep_ = 0.07f;   // +0x0c / +0x10 / +0x14
    float textAlpha_ = 0.f, textTarget_ = 1.0f, textStep_ = 0.1f;        // +0x18 / +0x1c / +0x20
    int pauseTime_ = 20;          // +0x48
    bool visible_ = false;
};

// The engine edges cGenericPlayerMiniGameTask touches, injected so the base stays self-contained.
struct Host {
    std::function<bool()> playerInVehicle;   // cPed::Vehicle() != null, for IsValid
    std::function<int()> runningAppType;     // Gui::cPda::RunningAppType, for OnEndApp / the Was* queries
    std::function<void(int ePdaApp)> loadApp;// Gui::cPda::LoadApp, OnEndApp returns to kHudApp
    std::function<void(Result)> complete;    // the cCallBack fired by CallTaskCompleteCallback with the result
    std::function<void()> closeAndLoadHud;   // PDAImpl::CloseMiniGamesAndLoadHud, used by the Cleanup path
};

// The reproduced cGenericPlayerMiniGameTask: the shared minigame task/lifecycle. A concrete minigame supplies its
// ePdaApp id and the player-status requirement, drives its own content, then sets the result and finishes.
class Minigame {
public:
    // Mirrors the ctor at 0x0070d4c4: app id (+0x18, via iAppTask), the player-status requirement (+0x2c), and
    // the host edges (the cCallBack + PDA hooks). hasCallback marks +0x40 (whether CallTaskCompleteCallback fires
    // the callback). The result (+0x5c) starts at Passed, matching mov w9,#1.
    Minigame(int ePdaApp, PlayerStatus status, Host host, bool hasCallback = true)
        : appId_(ePdaApp), status_(status), host_(std::move(host)), hasCallback_(hasCallback) {}

    int appId() const { return appId_; }
    PlayerStatus status() const { return status_; }

    // cGenericPlayerMiniGameTask::IsValid (0x0070d8a4). inVehicle is cPed::Vehicle() != null for the player's ped
    // (taken from host.playerInVehicle when omitted).
    bool isValid(bool inVehicle) const;
    bool isValid() const { return isValid(host_.playerInVehicle ? host_.playerInVehicle() : false); }

    // cGenericPlayerMiniGameTask::OnFinished (0x0070d8fc): the task is finished when the reason code is non-zero.
    static bool onFinished(int reason) { return reason != 0; }

    // Set the outcome stored at +0x5c.
    void setResult(Result r) { result_ = r; }
    Result result() const { return result_; }

    // cGenericPlayerMiniGameTask::CallTaskCompleteCallback (0x0070d600): invoke the stored callback with the
    // result (the original masks the handle with 0x3fffffff and then releases the weak pointer; here we just fire
    // host.complete when a callback is present, +0x40).
    void callTaskCompleteCallback();

    // cGenericPlayerMiniGameTask::OnEndApp (0x0070d908): unless auto-return is suppressed (+0x55) and the PDA is
    // still running this task's app, send the PDA back to the HUD app (LoadApp(0x10)).
    void onEndApp();

    // cGenericPlayerMiniGameTask::Abort (0x0070d964): clear the suppress-return flag (+0x55) and abort. (The
    // iAppTask::Abort tail is engine-side.)
    void abort() { suppressReturn_ = false; aborted_ = true; }

    // The suppress-auto-return-to-HUD flag at +0x55 (set by the owner when it will route the PDA itself).
    void setSuppressHudReturn(bool on) { suppressReturn_ = on; }
    bool suppressHudReturn() const { return suppressReturn_; }
    bool aborted() const { return aborted_; }

    // PDAImpl's result queries (0x008b2a34 / 0x008b2aa4 / 0x008b29c4): true only while this task is the running
    // minigame app (RunningAppType() >= 0x34 and it is this app) and the result matches. hasTask stands in for
    // cPlayer::HasMiniGameTask().
    bool wasPassed(bool hasTask) const { return isLiveQuery(hasTask) && result_ == Result::Passed; }
    bool wasFailed(bool hasTask) const { return isLiveQuery(hasTask) && result_ == Result::Failed; }
    bool wasQuit(bool hasTask) const { return isLiveQuery(hasTask) && result_ == Result::Quit; }

private:
    bool isLiveQuery(bool hasTask) const;

    int appId_ = 0;                          // +0x18 (iAppTask ePdaApp)
    PlayerStatus status_ = PlayerStatus::Any;// +0x2c
    Result result_ = Result::Passed;         // +0x5c (ctor = 1)
    bool suppressReturn_ = false;            // +0x55
    bool aborted_ = false;
    bool hasCallback_ = true;                // +0x40
    Host host_;                              // +0x30/+0x38 callback + the PDA edges
};

}  // namespace minigame
