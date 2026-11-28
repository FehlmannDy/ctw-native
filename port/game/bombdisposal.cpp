// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Gui::cBombDisposalApp puzzle logic. See bombdisposal.h for the mechanic map and the per-routine evidence. Only
// the self-contained state machine + validation math is reproduced here; rendering/camera/GXT are hooks.
#include "bombdisposal.h"

// cBombDisposalApp::Init (0x007e6a28): reset the app, pick the correct wire = Rand32NonCritical(3) (+0x204,
// 0x007e6b74). The ctor (0x007e6918) zeroes the phase/flags; Init then also shows GXT help 0x17e ("unscrew the
// panel") - a reported hook, not drawn here.
void BombDisposal::start(const std::function<int()>& rand3, std::function<void(bool)> done) {
    active_ = true;
    phase_ = kUnscrewing;
    screwsOut_ = 0;
    selected_ = 0;
    isFinished_ = false;
    finishCode_ = 0;
    success_ = false;
    finishCalled_ = false;
    done_ = std::move(done);

    int w = rand3 ? rand3() : 0;
    // Rand32NonCritical(3) yields [0,3); clamp defensively so a stray generator can never pick an impossible wire.
    if (w < 0) w = 0;
    if (w >= kWireCount) w = kWireCount - 1;
    correctWire_ = w;
}

// One screw fully turned. Models the point in Process (0x007e87e4) where ProcessRotatingSprite reports the current
// screw done: the screw pops (SFX 0x189) and the index advances. When the last screw pops, +0x1b4 leaves 0 and the
// reveal transition begins.
bool BombDisposal::turnScrew() {
    if (!active_ || phase_ != kUnscrewing) return false;
    if (screwsOut_ >= kScrews) return false;
    ++screwsOut_;
    if (playSfx) playSfx(kSfxScrewPop, 0x7f);
    if (screwsOut_ >= kScrews) {
        phase_ = kRevealing1;   // +0x1b4: 0 -> 1 (HideScrewVariants, Process 0x007e87f4)
        return true;
    }
    return false;
}

// cBombDisposalApp::Process reveal steps: +0x1b4 1 -> 2 (show wire cutter + GXT help 0x17f) -> 3 (cutting enabled).
void BombDisposal::processReveal() {
    if (!active_) return;
    if (phase_ == kRevealing1) phase_ = kRevealing2;
    else if (phase_ == kRevealing2) phase_ = kCutting;
}

// ProcessController wire-move clamp (0x007e7e50 / 0x007e7e88): left only if sel>=1, right only if sel<=1, so the
// selection stays within 0..kWireCount-1. SelectWire (0x007e7f6c) then refreshes the highlight sprites.
void BombDisposal::moveSelection(int dir) {
    if (!active_ || phase_ != kCutting) return;
    if (dir < 0) { if (selected_ >= 1) --selected_; }
    else if (dir > 0) { if (selected_ <= kWireCount - 2) ++selected_; }
}

void BombDisposal::setSelected(int wire) {
    if (!active_ || phase_ != kCutting) return;
    if (wire < 0) wire = 0;
    if (wire >= kWireCount) wire = kWireCount - 1;
    selected_ = wire;
}

bool BombDisposal::cutSelected() {
    if (!active_ || phase_ != kCutting) return false;
    return selectWireCut(selected_);
}

// cBombDisposalApp::SelectWireCut (0x007e8040), gameplay tail (0x007e8308): play the cut SFX, set the success byte
// +0x20f from (wire == correct wire +0x204), then finish once. Both outcomes end the minigame.
bool BombDisposal::selectWireCut(int wire) {
    if (!active_ || phase_ != kCutting) return false;
    if (finishCalled_) return success_;
    if (playSfx) playSfx(kSfxWireCut, 0x7f);          // 0x007e8080
    bool correct = (wire == correctWire_);            // 0x007e8308
    success_ = correct;                               // +0x20f (0x007e8320 / 0x007e8378)
    finish(correct);
    return correct;
}

// cBombDisposalApp::SetFinished (0x007e9f78): the explicit end used by the exit/epilogue path. Shares the one-shot
// completion guard +0x210. On a win the binary writes task +0x5c = 2 before CallTaskCompleteCallback; the port
// carries that through the done(win) callback.
bool BombDisposal::setFinished(bool win) {
    if (!active_) return false;
    success_ = win;
    return finish(win);
}

// cBombDisposalApp::EndApp (0x007e9d20): strh 0x0101 at +0x20d -> IsFinished = 1, FinishCode = 1.
void BombDisposal::endApp() {
    isFinished_ = true;
    finishCode_ = 1;
}

// The shared completion path guarded by +0x210 (SelectWireCut 0x007e8390.. / SetFinished 0x007e9f88..). In the
// binary this also checks the screen fade and cPlayer::HasMiniGameTask; here it simply fires the done hook once.
bool BombDisposal::finish(bool win) {
    if (finishCalled_) return false;   // +0x210 guard
    finishCalled_ = true;
    isFinished_ = true;                // EndApp-style: the app is now finished
    finishCode_ = 1;
    if (done_) done_(win);
    return true;
}
