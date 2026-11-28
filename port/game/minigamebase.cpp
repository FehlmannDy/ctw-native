// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
#include "minigamebase.h"

namespace minigame {

// MiniGameTimerBar::Update (0x00805048): ++elapsed (+0x20), then cset w0, ls on cmp(limit, elapsed) -- i.e.
// expired once the limit is at or below the new elapsed count.
bool TimerBar::update() {
    ++elapsed_;
    return limit_ <= elapsed_;
}

// cMiniGameResultTextHelper::Update (0x0081004c). The phase field (+0x08) runs 1 -> 2 -> 3 -> 4. Each frame one
// alpha ramp steps toward its target in the step's sign; the final applied sprite alpha is always the text alpha
// (+0x18). Returns true only from the Done phase.
bool ResultText::update() {
    bool done = false;
    switch (phase_) {
    case Phase::FadeIn:   // state 1: the black-poly backing alpha ramps to its target, then the text shows
        blackAlpha_ += blackStep_;
        if ((blackStep_ >= 0.f && blackAlpha_ > blackTarget_) ||
            (blackStep_ < 0.f && blackAlpha_ < blackTarget_)) {
            blackAlpha_ = blackTarget_;
            visible_ = true;
            if (onShow) onShow(true);    // ShowSprite(true) on both result windows (0x810124 / 0x810138)
            phase_ = Phase::Show;
        }
        break;
    case Phase::Show:     // state 2: the text alpha ramps 0 -> 1
        textAlpha_ += textStep_;
        if ((textStep_ >= 0.f && textAlpha_ > textTarget_) ||
            (textStep_ < 0.f && textAlpha_ < textTarget_)) {
            textAlpha_ = textTarget_;
            phase_ = Phase::Pause;
        }
        break;
    case Phase::Pause:    // state 3: hold for the pause time (+0x48), then done on the frame it goes negative
        if (--pauseTime_ < 0) {
            phase_ = Phase::Done;
            done = true;
        }
        break;
    case Phase::Done:     // state 4: stays done
        done = true;
        break;
    case Phase::Idle:
        break;
    }
    // Both branches fall through to pushing the text alpha (+0x18) to the sprite windows (0x810158..0x810180).
    if (onAlpha) onAlpha(textAlpha_);
    return done;
}

// cGenericPlayerMiniGameTask::IsValid (0x0070d8a4): status 0 is always valid; status 1 (OnFoot) is valid only
// when the player's ped has no vehicle; status 2 (InVehicle) only when it does.
bool Minigame::isValid(bool inVehicle) const {
    switch (status_) {
    case PlayerStatus::Any:       return true;
    case PlayerStatus::OnFoot:    return !inVehicle;
    case PlayerStatus::InVehicle: return inVehicle;
    }
    return true;
}

// cGenericPlayerMiniGameTask::CallTaskCompleteCallback (0x0070d600): if a callback object is present, invoke it.
// The original passes the task's weak-proxy handle (+0x38 masked with 0x3fffffff) and then (gated by +0x40)
// releases the weak pointer; the pass/fail outcome is read separately by the PDAImpl Was* queries from +0x5c.
// ASSUMPTION/port deviation: the port has no weak-proxy system, so it forwards the Result to the completion hook
// for the caller's convenience; the faithful pass/fail readback remains wasPassed()/wasFailed()/wasQuit().
void Minigame::callTaskCompleteCallback() {
    if (hasCallback_ && host_.complete) host_.complete(result_);
}

// cGenericPlayerMiniGameTask::OnEndApp (0x0070d908): if auto-return is not suppressed (+0x55 == 0) and the PDA is
// still showing this task's app, send the PDA back to the HUD app (LoadApp(ePdaApp 0x10)).
void Minigame::onEndApp() {
    if (suppressReturn_) return;
    const int running = host_.runningAppType ? host_.runningAppType() : 0;
    if (running == appId_ && host_.loadApp) host_.loadApp(kHudApp);
}

// Shared gate for the PDAImpl Was* queries (0x008b2a34 et al.): the running ePdaApp must be a minigame app
// (>= 0x34), the player must have a live minigame task, and -- so the readback is for this task -- the running
// app must be this one.
bool Minigame::isLiveQuery(bool hasTask) const {
    const int running = host_.runningAppType ? host_.runningAppType() : 0;
    return isMinigameApp(running) && running == appId_ && hasTask;
}

}  // namespace minigame
