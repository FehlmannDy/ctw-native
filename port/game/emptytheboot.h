// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Gui::cEmptyTheBootApp, the PDA minigame of emptying (and torching) a car boot/trunk full of incriminating
// papers: swipe the boot open, drag all the papers out one by one, scrape a match alight, and let the fire burn.
// Matched routine-by-routine against the Android ARM64 libGame.so (cEmptyTheBootApp, 0x7fb0f4..0x7fdba0).
//
// This file is the SELF-CONTAINED state machine + scoring logic only. All sprite/window construction, the
// cInteractiveSpriteWindow drag regions, cAnimateSpriteWindow tweens, HUD help text, rendering and the raw audio
// channels live in the engine's PDA/UI layer and are reported as hooks (see the lane report), not reproduced here.
// The one-shot HUD sfx are surfaced through the playSfx hook; the looping crackle/burn sfx are surfaced as the
// matchLoopPlaying()/fireLoopPlaying() flags (ProcessSfxLoops).
//
// The mechanic, as observed in the binary (there is NO timer and NO fail path; the only non-success exit is the
// exit button, i.e. the player giving up):
//   state 0  Closed:     a flick (DragDirection.y >= 1) opens the boot (cEmptyTheBootApp::OpenBoot).
//   state 1  Opening:     one settle frame, then cEmptyTheBootApp::EnableBoot arms the top paper.
//   state 2  Papers:      drag each of the 6 papers to the target to remove it (cEmptyTheBootApp::Paper_OnTarget);
//                         when the last paper is gone, cEmptyTheBootApp::ShowMatches advances to the match.
//   state 3  MatchSetup:  one frame of match-window setup in Process, then -> MatchScrape.
//   state 4  MatchScrape: scrape the match far enough along the box to light it (cEmptyTheBootApp::LightMatch).
//   state 5  MatchThrow:  the lit match tweens onto the papers; cEmptyTheBootApp::StartFire -> Fire.
//   state 6  Fire:        the papers burn (cEmptyTheBootApp::UpdateFire, 6 stages x 31 frames); done.
//
// Binary evidence for each constant is on the member that uses it below (observation unless marked assumption).
#pragma once
#include <cstdint>
#include <functional>

namespace etb {

// cEmptyTheBootApp state field +0x190. The integer values are the binary's own (0..6).
enum class State : int {
    Closed      = 0,   // swipe up to open the boot
    Opening     = 1,   // OpenBoot ran; one settle frame then EnableBoot
    Papers      = 2,   // drag the papers out
    MatchSetup  = 3,   // ShowMatches set this; Process settles it to MatchScrape next frame
    MatchScrape = 4,   // scrape the match to light it
    MatchThrow  = 5,   // the lit match tweens onto the papers
    Fire        = 6,   // the papers burn, then the app returns to the HUD
};

class EmptyTheBoot {
public:
    // ---- constants re-derived from libGame.so (all offsets are into the cEmptyTheBootApp object) --------------
    // Papers to empty. The ctor (0x7fb0f4) sets +0x1fc from the 8-byte constant at 0x47af68 (ff 00 00 00 06 00 00
    // 00): the low word 0xff is the match-loop sfx tracker (+0x1f8), the high word 6 is the paper count (+0x1fc).
    // OpenBoot/ClearPaperOutlines address 6 paper windows (+0x1d0..+0x1d5) and 6 outline sprites (+0x1d6..+0x1db),
    // consistent with 6. (observation)
    static constexpr int kPapers = 6;

    // OpenBoot gate, Process state 0 (0x7fcb3c/0x7fcb58/0x7fcb64): cTouchScreen::DragMagnitude >= 0x29,
    // DragDirection.y >= 1, and the drag's direction word <= 0x1ff. (observation)
    static constexpr int kOpenDragMagMin = 0x29;   // 41
    static constexpr int kOpenDirYMin    = 1;
    static constexpr int kOpenAngleMax   = 0x1ff;

    // Opening settle: Process state 1 increments +0x1e0 and runs EnableBoot once the counter exceeds 1, i.e. on
    // the second frame in state 1 (0x7fcf70). (observation)
    static constexpr int kOpeningSettleFrames = 2;

    // Match scrape gate, Process state 4 (0x7fcbec/0x7fcc04): the frame-to-frame match travel distance must exceed
    // 0xf000 in 20.12, i.e. 15 px, with DragDirection.x >= 1. (observation)
    static constexpr int kScrapeDistMin = 15;      // 0xf000 >> 12
    static constexpr int kScrapeDirXMin = 1;

    // Fire burn, UpdateFire (0x7fd998): +0x1c0 counts to 0x1f (31) frames per stage; +0x1b8 steps 0->6 (6 stages),
    // and the burn is complete when it reaches 6. (observation)
    static constexpr int kFireStageFrames = 0x1f;  // 31
    static constexpr int kFireStages      = 6;

    // HUD / looping sfx ids (observation):
    static constexpr int kSfxOpenBoot   = 0x27b;   // OpenBoot (0x7fc358), one-shot
    static constexpr int kSfxScrape     = 0x2e6;   // match scrape (0x7fcc14 / ProcessController), one-shot, vol 0x7f
    static constexpr int kSfxScrapeVol  = 0x7f;
    static constexpr int kSfxFireLoop   = 0x1ed;   // ProcessSfxLoops (0x7fd568), looped while state==Fire, vol 0x32
    static constexpr int kSfxMatchLoop  = 0x25a;   // ProcessSfxLoops (0x7fd5cc), looped while the match is lit
    static constexpr int kSfxLoopVol    = 0x32;

    // cEmptyTheBootApp::Init / reset. done(success): success == the fire burned (the boot was emptied and torched);
    // !success == the player gave up via the exit button. The orchestrator supplies the callback.
    void start(std::function<void(bool success)> done = {});
    void stop();
    bool active() const { return active_; }

    // cEmptyTheBootApp::Process: one 30 Hz frame. Drives the time-based transitions (the Opening settle, the
    // MatchSetup frame, the MatchThrow tween and the Fire burn) and ProcessSfxLoops; input-based transitions come
    // through the methods below.
    void process();

    // state 0 input: a finished swipe. Runs OpenBoot when the drag clears the three gates. Returns true if it
    // opened the boot.
    bool swipeOpen(int dragMagnitude, int dragDirY, int dragAngle);

    // state 2 input: drop the current top paper on the target (cEmptyTheBootApp::Paper_OnTarget). Returns true if a
    // paper was removed. When the last paper is removed it runs ShowMatches (-> MatchSetup).
    bool dropPaper();

    // state 4 input: scrape the match (cEmptyTheBootApp::LightMatch when the scrape clears the gate). Returns true
    // if the match lit on this scrape.
    bool scrapeMatch(int dragDistance, int dragDirX);

    // The exit button (SS_AlwaysLoaded sprite 0x93, shown during the Papers phase): give up. done(false).
    void quit();

    // ---- observers -------------------------------------------------------------------------------------------
    State state() const { return state_; }
    int papersLeft() const { return papers_; }      // +0x1fc
    bool matchLit() const { return matchLit_; }      // +0x195
    int fireStage() const { return fireStage_; }     // +0x1b8
    bool finished() const { return finished_; }
    bool success() const { return success_; }
    // cEmptyTheBootApp::IsFinished (0x7fdb9c) is a stub that always returns false in the binary; mirrored here.
    bool isFinished() const { return false; }

    // ProcessSfxLoops state: the fire crackle loop runs while state==Fire; the match-burn loop while the match is
    // lit (and not yet consumed by the fire). These stand in for the +0x1f4/+0x1f8 channel trackers.
    bool fireLoopPlaying() const { return fireLoop_; }
    bool matchLoopPlaying() const { return matchLoop_; }

    // One-shot HUD sfx hook (cAudioManager::PlayHudSfx). Looping sfx are reported through the flags above.
    std::function<void(int sample, int volume)> playSfx;

private:
    void openBoot();         // cEmptyTheBootApp::OpenBoot
    void enableBoot();        // cEmptyTheBootApp::EnableBoot
    void showMatches();       // cEmptyTheBootApp::ShowMatches
    void lightMatch();        // cEmptyTheBootApp::LightMatch
    void startFire();         // cEmptyTheBootApp::StartFire
    bool updateFire();        // cEmptyTheBootApp::UpdateFire: returns false once the burn is complete
    void processSfxLoops();   // cEmptyTheBootApp::ProcessSfxLoops
    void finish(bool success);

    bool active_ = false;
    bool finished_ = false;
    bool success_ = false;
    State state_ = State::Closed;       // +0x190
    int papers_ = kPapers;              // +0x1fc
    bool matchLit_ = false;             // +0x195
    int openingCounter_ = 0;            // +0x1e0
    int fireStage_ = 0;                 // +0x1b8
    int fireCounter_ = 0;               // +0x1c0
    bool fireLoop_ = false;             // ProcessSfxLoops, +0x1f4 != 0xff
    bool matchLoop_ = false;            // ProcessSfxLoops, +0x1f8 != 0xff
    std::function<void(bool)> done_;

    friend struct EmptyTheBootTestAccess;
};

}  // namespace etb
