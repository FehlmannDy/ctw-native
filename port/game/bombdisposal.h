// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Gui::cBombDisposalApp: the PDA "defuse the bomb" minigame. Matched routine-by-routine against the Android
// ARM64 libGame.so (addresses cited below; "observation" unless marked "assumption"). This file reproduces the
// self-contained PUZZLE STATE MACHINE + VALIDATION MATH only; the sprite/window drawing, the camera, the GXT help
// text and the audio are left as reported hooks (see the std::function members and the comments), so the file is
// self-contained and needs no engine GL/asset context.
//
// Mechanic (observation, from Init/Process/ProcessController/SelectWireCut):
//   PHASE 1 - unscrew the faceplate.  Four corner screws (+0x1d8 is the current screw index; the i<4 loop in
//     cBombDisposalApp::Process, 0x007e8528, drives all four). A screw is turned with the screwdriver
//     (cBombDisposalApp::SelectScrew 0x007ea238 arms it at +0x1b8/+0x1ba/+0x1d8; ProcessAnalogRotation +
//     ProcessRotatingSprite 0x007e8f74 integrate the turn angle in 20.12 fixed point, quarter-turn threshold
//     0x4000, full turn 0xc000). Each completed screw pops out (SFX 0x189, Process 0x007e85f8). When the rotation
//     phase finishes, cBombDisposalApp::Process advances the phase field +0x1b4 (0 -> 1 -> 2 -> 3) through a short
//     reveal transition that swaps the screwdriver UI for the wire cutter (GXT help id 0x17f shown at +0x1b4==2,
//     Process 0x007e8778). NOTE: the exact per-screw angle integrator is NOT reproduced here (it is cosmetic to
//     win/lose); this port reproduces the four-screw count, the SFX and the phase progression gate.
//   PHASE 2 - cut the correct wire.  Three wires, indices 0..2 (SelectWire 0x007e7f6c highlights +0x1a5/+0x1a6/
//     +0x1a7; selection +0x1e8 is clamped to 0..2 in ProcessController 0x007e7d80: move left only if sel>=1, right
//     only if sel<=1). The CORRECT wire is chosen once in cBombDisposalApp::Init (0x007e6a28, 0x007e6b74) as
//     Rand32NonCritical(3), stored at +0x204. Cutting is enabled only once +0x1b4 == 3 (ProcessController
//     0x007e7e20). A confirm press calls SelectWireCut(selected, 0, 1) (ProcessController 0x007e7f4c; a touch sets
//     +0x1ae and Process 0x007e84f0 dispatches the same).
//
// Win/lose (observation, cBombDisposalApp::SelectWireCut 0x007e8040):
//   - plays the cut SFX 0x152 (0x007e8080),
//   - compares the cut wire against the correct wire +0x204 (0x007e8308): a match sets the success byte +0x20f = 1
//     (0x007e8320), a mismatch sets +0x20f = 0 (0x007e8378),
//   - EITHER outcome ends the minigame: it waits for the screen fade to settle then calls
//     cGenericPlayerMiniGameTask::CallTaskCompleteCallback exactly once, guarded by +0x210 (0x007e8390..0x007e83e0;
//     cBombDisposalApp::SetFinished 0x007e9f78 shares that guard and, on a win, writes task +0x5c = 2).
//
// IMPORTANT finding (observation, reported, not an omission): there is NO countdown timer and NO "wrong-move
// penalty / retry" in cBombDisposalApp. All 27 routines were inspected; none is a timer (unlike cHotWiringApp's
// MiniGameTimerBar). Cutting the WRONG wire does not deduct time or let play continue - it ends the game
// immediately as a failure. So "lose on timeout" does not apply; the loss condition is "cut the wrong wire". The
// armed-beep SFX 0x2b8 (PlayArmedBeepSfx 0x007e8a24) and the volt loop (PlayVoltLoop) are ambience, not a clock.
//
// Flags: cBombDisposalApp::IsFinished 0x007e9f68 -> +0x20d; FinishCode 0x007e9f70 -> +0x20e;
//        EndApp 0x007e9d20 writes the halfword 0x0101 at +0x20d (IsFinished=1, FinishCode=1).
#pragma once
#include <cstdint>
#include <functional>

class BombDisposal {
public:
    // The phase field +0x1b4 plus the overall lifecycle. The names mirror what each +0x1b4 value drives in
    // cBombDisposalApp::Process.
    enum Phase : int {
        kUnscrewing = 0,   // +0x1b4 == 0: turning the four faceplate screws
        kRevealing1 = 1,   // +0x1b4 == 1: reveal transition step (hide screwdriver)
        kRevealing2 = 2,   // +0x1b4 == 2: reveal transition step (show wire cutter + GXT help 0x17f)
        kCutting    = 3,   // +0x1b4 == 3: wire selection + cut enabled
    };

    static constexpr int kScrews    = 4;   // Process i<4 loop (0x007e8528) / +0x1d8 screw index
    static constexpr int kWireCount = 3;   // SelectWire highlights wires 0..2; correct = Rand32NonCritical(3)

    // SFX sample ids seen in the binary (exposed through playSfx; volumes as the binary passes them).
    static constexpr int kSfxScrewPop  = 0x189;  // Process 0x007e85f8 (a screw pops out), vol 0x7f
    static constexpr int kSfxWireCut   = 0x152;  // SelectWireCut 0x007e8080, vol 0x7f
    static constexpr int kSfxArmedBeep = 0x2b8;  // PlayArmedBeepSfx 0x007e8a58, vol 0x28

    // cBombDisposalApp::Init (0x007e6a28). rand3 must return a value in [0,3) (the binary uses
    // Rand32NonCritical(3)); pass a deterministic generator in tests. done(success) is the completion hook
    // (CallTaskCompleteCallback): success == "cut the correct wire".
    void start(const std::function<int()>& rand3, std::function<void(bool success)> done = {});

    bool active() const { return active_; }
    int  phase() const { return phase_; }                 // +0x1b4
    int  correctWire() const { return correctWire_; }     // +0x204
    int  selected() const { return selected_; }           // +0x1e8
    int  screwsOut() const { return screwsOut_; }
    bool cutting() const { return phase_ == kCutting; }    // wire cut enabled (ProcessController 0x007e7e20)

    // PHASE 1. Complete the turn of the current screw (one full turn, SFX 0x189). Only acts while kUnscrewing.
    // Returns true when that turn pops the last screw and the reveal begins. (Models the point where
    // ProcessRotatingSprite reports the screw done and Process advances +0x1b4.)
    bool turnScrew();
    // One Process frame of the screwdriver->cutter reveal (steps +0x1b4 1 -> 2 -> 3). No-op outside the reveal.
    void processReveal();

    // PHASE 2. Move the wire selection (dir < 0 left, dir > 0 right), clamped to 0..kWireCount-1 exactly as
    // ProcessController (left only if sel>=1, right only if sel<=1). No-op unless cutting().
    void moveSelection(int dir);
    // Select a specific wire directly (clamped 0..2). No-op unless cutting().
    void setSelected(int wire);

    // cBombDisposalApp::SelectWireCut (0x007e8040) for the currently selected wire: the confirm press. Plays the
    // cut SFX, records success (+0x20f = cut==correct), and ends the game once (SetFinished guard). No-op unless
    // cutting() and not already finished. Returns the success flag.
    bool cutSelected();
    // Cut an explicit wire index (what ProcessController/Process pass as the selected wire). Clamped is NOT applied
    // here (the binary cuts exactly the index given); out-of-range simply never matches the correct wire.
    bool selectWireCut(int wire);

    // cBombDisposalApp::SetFinished (0x007e9f78): end the game with an explicit result, guarded so it fires once.
    // Used by the exit/epilogue paths. Returns true if this call is the one that finished it.
    bool setFinished(bool win);
    // cBombDisposalApp::EndApp (0x007e9d20): set IsFinished=1 and FinishCode=1.
    void endApp();

    bool isFinished() const { return isFinished_; }  // cBombDisposalApp::IsFinished, +0x20d
    int  finishCode() const { return finishCode_; }  // cBombDisposalApp::FinishCode, +0x20e
    bool success() const { return success_; }        // +0x20f: the player cut the correct wire
    bool completed() const { return finishCalled_; } // +0x210: the completion callback has fired

    // Audio hook (cAudio/cAudioManager::PlaySfx). Left unset in tests that only check state.
    std::function<void(int sample, int volume)> playSfx;

private:
    bool finish(bool win);   // the shared "complete once" path (SelectWireCut tail / SetFinished), guard +0x210

    bool active_ = false;
    int  phase_ = kUnscrewing;   // +0x1b4
    int  screwsOut_ = 0;         // screws popped so far (0..kScrews)
    int  correctWire_ = 0;       // +0x204, Rand32NonCritical(3)
    int  selected_ = 0;          // +0x1e8 (binary starts -1/0xff then clamps to 0 on first use)
    bool isFinished_ = false;    // +0x20d
    int  finishCode_ = 0;        // +0x20e
    bool success_ = false;       // +0x20f
    bool finishCalled_ = false;  // +0x210 (one-shot completion guard)
    std::function<void(bool)> done_;

    friend struct BombDisposalTestAccess;
};
