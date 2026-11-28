// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Gui::cEmptyTheBootApp state machine + scoring logic, matched against libGame.so (0x7fb0f4..0x7fdba0). See
// emptytheboot.h for the mechanic and the per-constant binary evidence. This file reproduces only the self-
// contained integer logic; sprites/windows/tweens/HUD/audio-channels are engine hooks (reported in the lane note).
#include "emptytheboot.h"
#include <utility>   // std::move

namespace etb {

// cEmptyTheBootApp::cEmptyTheBootApp (0x7fb0f4) + Init (0x7fb1dc) reset: state 0, 6 papers, nothing lit or burning.
void EmptyTheBoot::start(std::function<void(bool success)> done) {
    done_ = std::move(done);
    active_ = true;
    finished_ = false;
    success_ = false;
    state_ = State::Closed;
    papers_ = kPapers;          // +0x1fc = 6
    matchLit_ = false;          // +0x195
    openingCounter_ = 0;        // +0x1e0
    fireStage_ = 0;             // +0x1b8
    fireCounter_ = 0;           // +0x1c0
    fireLoop_ = false;          // +0x1f4 sentinel
    matchLoop_ = false;         // +0x1f8 sentinel
}

void EmptyTheBoot::stop() {
    active_ = false;
    fireLoop_ = false;
    matchLoop_ = false;
}

void EmptyTheBoot::finish(bool success) {
    if (finished_) return;
    finished_ = true;
    success_ = success;
    // Leaving the app stops both looping channels (DeInit / ProcessSfxLoops).
    fireLoop_ = false;
    matchLoop_ = false;
    if (done_) done_(success);
}

// cEmptyTheBootApp::Process (0x7fc8a0): the time-driven transitions and ProcessSfxLoops. Input-driven transitions
// (OpenBoot, Paper_OnTarget, LightMatch) arrive through swipeOpen/dropPaper/scrapeMatch.
void EmptyTheBoot::process() {
    if (!active_ || finished_) return;

    switch (state_) {
        case State::Closed:
            // Waiting for a flick (swipeOpen); nothing time-driven here.
            break;

        case State::Opening:
            // Process state 1 (0x7fcf70): the boot-open frame counter +0x1e0 steps up; once it exceeds 1 (the
            // second frame) EnableBoot arms the papers. (The binary also waits for touch release via a global; the
            // port has no touch global, so it settles purely on the frame count.)
            ++openingCounter_;
            if (openingCounter_ > 1) enableBoot();
            break;

        case State::Papers:
            // Process state 2 has no body; paper removal is driven by dropPaper(). Nothing time-driven here.
            break;

        case State::MatchSetup:
            // Process state 3 (0x7fc904) sets up the match window and falls straight to state 4 in the same frame.
            state_ = State::MatchScrape;
            break;

        case State::MatchScrape:
            // Process state 4 (0x7fce14): once the match is lit, advance to the throw. (LightMatch set matchLit_.)
            if (matchLit_) state_ = State::MatchThrow;
            break;

        case State::MatchThrow:
            // The lit match tweens onto the papers (cAnimateSpriteWindow, engine hook); when it lands, the animate
            // callback StartFire runs. The port collapses the tween to the next frame.
            startFire();
            break;

        case State::Fire:
            // Process state 6 (0x7fcabc) hands control back to the HUD (cPda::LoadApp 0x10) while the fire burns
            // over the transition (UpdateFire). The port signals completion when the burn is fully grown.
            if (!updateFire()) finish(true);
            break;
    }

    processSfxLoops();
}

// cEmptyTheBootApp::OpenBoot (0x7fc344): sfx 0x27b, reveal the papers, state 0 -> 1.
void EmptyTheBoot::openBoot() {
    if (playSfx) playSfx(kSfxOpenBoot, 0x64);
    openingCounter_ = 0;        // +0x1e0 = 0
    state_ = State::Opening;    // +0x190 = 1
}

bool EmptyTheBoot::swipeOpen(int dragMagnitude, int dragDirY, int dragAngle) {
    if (!active_ || finished_ || state_ != State::Closed) return false;
    // Process state 0 gate (0x7fcb3c/0x7fcb58/0x7fcb64).
    if (dragMagnitude < kOpenDragMagMin) return false;
    if (dragDirY < kOpenDirYMin) return false;
    if (dragAngle > kOpenAngleMax) return false;
    openBoot();
    return true;
}

// cEmptyTheBootApp::EnableBoot (0x7fd300): arm the top paper as a drag target, state 1 -> 2.
void EmptyTheBoot::enableBoot() {
    state_ = State::Papers;     // +0x190 = 2
}

// cEmptyTheBootApp::Paper_OnTarget (0x7fc478): remove the current top paper; when the last is gone, ShowMatches.
bool EmptyTheBoot::dropPaper() {
    if (!active_ || finished_ || state_ != State::Papers) return false;
    if (papers_ <= 0) return false;
    --papers_;                  // +0x1fc--
    if (papers_ == 0) {
        showMatches();          // the boot is empty
    }
    // Otherwise the next top paper is re-armed (engine window setup); nothing else to track here.
    return true;
}

// cEmptyTheBootApp::ShowMatches (0x7fd628): reveal the matchbox + help text 0x1a9, state 2 -> 3.
void EmptyTheBoot::showMatches() {
    state_ = State::MatchSetup; // +0x190 = 3
}

// cEmptyTheBootApp::LightMatch (0x7fc208): light the match on a hard-enough scrape.
void EmptyTheBoot::lightMatch() {
    if (matchLit_) return;      // no-op if already lit (0x7fc208)
    matchLit_ = true;           // +0x195 = 1
    fireStage_ = 0;             // (the flame frame +0x1ac is reset; fire stage is reset at StartFire)
    fireCounter_ = 0;           // +0x1c0 = 0
}

bool EmptyTheBoot::scrapeMatch(int dragDistance, int dragDirX) {
    if (!active_ || finished_ || state_ != State::MatchScrape) return false;
    // Process state 4 scrape gate (0x7fcbec/0x7fcc04): travel > 15 px with DragDirection.x >= 1.
    if (dragDistance <= kScrapeDistMin) return false;
    if (dragDirX < kScrapeDirXMin) return false;
    const bool wasLit = matchLit_;
    if (playSfx) playSfx(kSfxScrape, kSfxScrapeVol);   // PlayMatchScrapeSfx (0x7fc1f4)
    lightMatch();
    return !wasLit && matchLit_;
}

// cEmptyTheBootApp::StartFire (0x7fc2a8): the lit match landed; begin the burn, state 5 -> 6.
void EmptyTheBoot::startFire() {
    fireCounter_ = 0;           // +0x1c0 = 0
    fireStage_ = 0;             // +0x1b8 = 0
    state_ = State::Fire;       // +0x190 = 6
}

// cEmptyTheBootApp::UpdateFire (0x7fd998): step the burn. 31 frames per stage, 6 stages; returns false when the
// fire is fully grown (stage reaches 6).
bool EmptyTheBoot::updateFire() {
    ++fireCounter_;             // +0x1c0++
    if (fireCounter_ >= kFireStageFrames) {   // >= 0x1f
        fireCounter_ = 0;
        ++fireStage_;           // +0x1b8++
        if (fireStage_ >= kFireStages) return false;   // stage == 6: done
    }
    return true;
}

// cEmptyTheBootApp::ProcessSfxLoops (0x7fd538): the fire crackle loop runs while in Fire; the match-burn loop runs
// while the match is lit (and not yet consumed). 0xff is the "no channel" sentinel; here these are bool flags.
void EmptyTheBoot::processSfxLoops() {
    const bool wantFire = (state_ == State::Fire) && !finished_;
    fireLoop_ = wantFire;                 // +0x1f4: play while state==6, else stop
    const bool wantMatch = matchLit_ && !finished_;
    matchLoop_ = wantMatch;               // +0x1f8: play while +0x195 set, else stop
}

// OnExitButtonPress: the exit button (SS_AlwaysLoaded 0x93) gives up without torching the boot.
void EmptyTheBoot::quit() {
    if (!active_ || finished_) return;
    finish(false);
}

}  // namespace etb
