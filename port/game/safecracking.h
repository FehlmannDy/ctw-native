// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Gui::cSafeCrackingApp: the PDA safe-cracking minigame. The player cracks a 3-number combination by rotating a
// dial (a cInteractiveSprite the hand grabs) and listening to a stethoscope. The dial has ten notches (0..9). For
// each tumbler the player turns the dial in the required direction until the current notch matches the hidden
// target, then releases to lock it in. A stethoscope meter and a "click" give the feedback as the notch nears the
// target. Three correct releases open the safe.
//
// This module reproduces the self-contained puzzle state machine and the dial / tolerance math only. The sprite
// layout, the cInteractiveSprite drag region, the camera, GXT help text and the PDA frame are UI and are reported
// as hooks (see safecracking.cpp), not drawn here. Verified routine-by-routine against the Android ARM64
// libGame.so (addresses cited per field / routine; "observation" unless marked "assumption").
//
// Combination / dial model (observation):
//   - 3 tumblers, targets at +0x1bb / +0x1bc / +0x1bd (cSafeCrackingApp::FindWantedRots 0x81d33c).
//   - 10 notches: a notch is 65536/10 = 6553 (0x1999) of a 16-bit sprite rotation; the half-notch rounding offset
//     0xccc (=3276) gives the "sweet spot" tolerance of +/-0x0ccc (~+/-18 deg) for the notch to read as the target
//     (cSafeCrackingApp::OnRelease 0x81ed44 and the per-frame block in cSafeCrackingApp::Process 0x81d4f0).
//   - Per-tumbler turn direction alternates CCW / CW / CCW (ProcessAnalogRotation arg 2 / 1 / 2, set at the Process
//     step-2/3/4 handlers). The stetho distance is measured in that direction (+0x1cb, 0=CCW path, 1=CW path).
//   - Stetho meter 0..7 from a cubic of the wrapped distance, 3*(dist/10)^3 scaled, forced to 7 at the exact notch
//     (cSafeCrackingApp::UpdateStethoTarget 0x81eaf0; ProcessStethoScale 0x81eb94 animates it +/-1 a frame).
//   - There is no timer and no fail state: a wrong release just leaves the tumbler to retry (OnRelease).
//   - Win: the third correct release -> outro slide (step 5) -> the dial spins the safe open (step 6, sfx 0x2e5) ->
//     a 45-frame (0x2d) wait -> SetFinished (+0x1a5 = 1) + the player's minigame-complete callback (0x81ec74).
#pragma once
#include <array>
#include <cstdint>
#include <functional>

namespace safecracking {

// cSafeCrackingApp +0x1ac step id. Steps 2/3/4 are the three tumblers; 0/1 intro, 5/6 win.
enum Step : int {
    kStepPreIntro = 0,   // nothing yet (ctor state, +0x1ac = 0)
    kStepIntro    = 1,   // the frame/windows slide in (+0x2ac counts down from 0x1d4 by 0x16)
    kStepTumbler0 = 2,   // first tumbler  (CCW)
    kStepTumbler1 = 3,   // second tumbler (CW)
    kStepTumbler2 = 4,   // third tumbler  (CCW)
    kStepWinOutro = 5,   // the frame slides back out (+0x2ac counts up to 0x1d4)
    kStepWinSpin  = 6,   // the dial spins the safe open, then SetFinished
};

// Fixed-point / combination constants read from the binary.
constexpr int kNotches       = 10;       // ten dial positions 0..9 (the /10 in OnRelease / Process)
constexpr int kNotchStep     = 0x1999;   // 6553, one notch of a 16-bit rotation (SetSpriteRotation(notch*0x1999))
constexpr int kNotchRound    = 0xccc;    // 3276, the half-notch rounding = the sweet-spot tolerance (+/-)
constexpr int kTumblers      = 3;        // the combination length (three wanted rots)
constexpr int kStethoMax     = 7;        // the stetho meter tops out at 7 bars (ProcessStethoScale 7 sprites)
constexpr int kDoorOpenDelay = 0x2d;     // 45 frames from the safe opening to SetFinished (Process 0x81e250)
constexpr int kIntroTotal    = 0x1d4;    // 468, the intro / outro slide span (+0x2ac)
constexpr int kIntroStep     = 0x16;     // 22, the per-frame slide amount

// Sound sample ids (cAudioManager::PlaySfx, volume 0x7f, channel 2). UI hooks; fired through playSfx.
enum Sfx : int {
    kSfxAtTarget  = 0x2e0,   // the notch reaches the target (stetho hits 7)      Process / UpdateStethoTarget
    kSfxTick      = 0x2e1,   // the dial clicks past a notch (notch changed)      Process
    kSfxSucceeded = 0x2e2,   // cInteractiveSprite succeeded (gamepad path)       InteractiveSpriteSucceeded
    kSfxStarted   = 0x2e3,   // cInteractiveSprite grabbed                        InteractiveSpriteStarted
    kSfxLockIn    = 0x2e4,   // a correct release locks the tumbler in            OnRelease
    kSfxDoorOpen  = 0x2e5,   // the safe door opens                               Process (step 6)
};

// eRotationDirection passed to ProcessAnalogRotation (observation): 1 accepts only positive turn, 2 only negative.
enum Direction : int { kDirAny = 0, kDirPositive = 1, kDirNegative = 2 };

struct TestAccess;  // the unit test reaches internals through this friend

class SafeCracking {
public:
    SafeCracking() { reset(); }

    // cSafeCrackingApp::cSafeCrackingApp / Init: defaults, then arm the first tumbler. Pass a combination for a
    // deterministic game, or call findWantedRots() for the RNG one.
    void reset();
    void start(int target0, int target1, int target2);

    // cSafeCrackingApp::FindWantedRots (0x81d33c): roll the three targets. target0 = rng(7)+3 in [3,9]; the next two
    // are rng(10) rerolled until far enough from the previous notch in the turning direction (dir). rng(n) must
    // return a value in [0, n). Returns the rolled combination.
    std::array<int, 3> findWantedRots(const std::function<uint32_t(uint32_t)>& rng, int dir = 0);

    // Set the live dial angle (what ProcessAnalogRotation accumulates into the sprite's int16 rotation). Positive
    // and negative wrap through the 16-bit range exactly like the sprite's stored rotation.
    void setRotation(int angle) { rotation_ = static_cast<int16_t>(angle); }
    void rotateBy(int delta)    { rotation_ = static_cast<int16_t>(rotation_ + delta); }
    int  rotation() const       { return rotation_; }

    // cSafeCrackingApp::Process (0x81d4f0): advance one 30 Hz frame. During a tumbler it reads the notch, updates
    // the at-target flag, the stetho, and the tick / sweet-spot sounds; during intro / win it steps those timers.
    void process();

    // cSafeCrackingApp::OnRelease (0x81ed44): the player let go of the dial. On the exact target notch this locks
    // the tumbler and advances (the third advances into the win sequence); otherwise it is a no-op retry.
    void onRelease();

    // ----- the dial / tolerance math (static, re-derived from the binary's integer ops) -------------------------
    // The current dial notch 0..9 from a 16-bit rotation, with the half-notch rounding (Process / OnRelease).
    static int notchFromRotation(int angle);
    // The wrapped notch distance cur->target measured in the turning direction (dir 0 = CCW path, else CW path),
    // 0..9 (cSafeCrackingApp::UpdateStethoTarget 0x81eaf0; the inline block in Process).
    static int wrappedDistance(int target, int cur, int dir);
    // The stetho meter target 0..~4 for a wrapped distance (the cube 3*(dist/10)^3 scaled). The game forces 7 at
    // the exact notch; this returns only the formula value (UpdateStethoTarget / Process).
    static int stethoFromDistance(int dist);
    // cSafeCrackingApp::GetDigitalNumberSpriteId (0x81ecc8): clamp n to 0..9, map to the digit sprite id n+11.
    static int getDigitalNumberSpriteId(int n);

    // ----- queries ---------------------------------------------------------------------------------------------
    int  step() const       { return step_; }
    int  tumbler() const    { return step_ - kStepTumbler0; }   // 0..2 while cracking, else out of range
    int  target() const     { return target_; }                 // +0x1ca, the active tumbler's target notch
    int  direction() const  { return dirFlag_ ? kDirPositive : kDirNegative; }
    int  currentNotch() const { return notchFromRotation(rotation_); }
    bool atTarget() const   { return atTarget_; }               // +0x1cc
    int  stethoTarget() const { return stethoTarget_; }         // +0x1cf
    int  stethoLevel() const  { return stethoLevel_; }          // +0x1ce, the animated displayed level
    bool finished() const   { return finishCode_ != 0; }        // cSafeCrackingApp::IsFinished (+0x1a5)
    int  finishCode() const { return finishCode_; }             // cSafeCrackingApp::FinishCode
    const std::array<int, 3>& combination() const { return combo_; }
    const std::array<int, 3>& lockedDigits() const { return locked_; }  // +0x1b8..+0x1ba, the shown digits

    // Hooks (reported, never faked). playSfx fires the UI sounds; onFinished is SetFinished's task-complete
    // callback (cGenericPlayerMiniGameTask::CallTaskCompleteCallback), a cross-file hook the orchestrator wires.
    std::function<void(int sample, int volume)> playSfx;
    std::function<void()> onFinished;
    // cContainerWnd::AddBgWindow for the opened-safe interior (step 6); the orchestrator draws it. Reported hook.
    std::function<void()> onDoorOpen;

private:
    friend struct TestAccess;
    void updateStethoTarget(int curNotch);   // UpdateStethoTarget
    void processStethoScale();                // ProcessStethoScale
    void nextStep();                          // NextStep (0x81d320)
    void armTumbler();                         // the Process step-2/3/4 first-entry target assignment
    void setFinished();                        // SetFinished (0x81ec74)
    void processTumbler();                     // the per-frame tumbler block shared by steps 2/3/4
    void sfx(int sample) { if (playSfx) playSfx(sample, 0x7f); }

    int step_ = kStepPreIntro;     // +0x1ac
    int16_t rotation_ = 0;         // the dial sprite's int16 rotation (+0x210 / +0x280 [0x26])
    std::array<int, 3> combo_{{0, 0, 0}};    // +0x1bb..+0x1bd, the wanted rots
    std::array<int, 3> locked_{{0, 0, 0}};   // +0x1b8..+0x1ba, the digits shown locked in
    int target_ = 0;               // +0x1ca, the active tumbler target notch
    int dirFlag_ = 0;              // +0x1cb, 0 = CCW distance rule, 1 = CW
    bool armed_ = false;           // +0x1b0, the step's one-time setup has run
    bool atTarget_ = false;        // +0x1cc, current notch == target this frame
    int stethoTarget_ = 0;         // +0x1cf
    int stethoLevel_ = 0;          // +0x1ce
    int lastNotch_ = -1;           // +0x2b2, the notch last shown (for the tick sound)
    int introTimer_ = 0;           // +0x2ac, the intro / outro slide counter
    int doorTimer_ = 0;            // +0x1a6 in step 6, frames since the door opened
    bool doorOpen_ = false;        // +0x1cd, the opened-safe window has been added
    int finishCode_ = 0;           // +0x1a5
};

}  // namespace safecracking
