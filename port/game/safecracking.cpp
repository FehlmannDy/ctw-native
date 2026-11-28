// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Gui::cSafeCrackingApp, the puzzle core. The dial / tolerance math and the tumbler state machine, re-derived
// instruction-by-instruction from the Android ARM64 libGame.so. Only the self-contained logic lives here; the
// sprite layout, the cInteractiveSprite drag region, the camera, GXT help text and the PDA chrome are UI and are
// reported as hooks (below), not reproduced.
//
// Cross-file / UI hooks this module needs the orchestrator to wire (reported, not faked, no dead code here):
//   - The dial itself is a cInteractiveSprite: cHotWiringApp-style cContainerWnd + cWnd::OnPress / OnRelease route
//     the touch and ProcessAnalogRotation (0x81e594) turns the maths::GetAngleBetweenLines delta (scaled by 0x6488)
//     into the sprite's int16 rotation. The port takes that rotation directly through setRotation()/rotateBy();
//     feeding it from a real touch is the orchestrator's job.
//   - playSfx -> cAudioManager::PlaySfx (sample, 0x7f, SOUNDChannel 2). onFinished -> SetFinished's
//     cGenericPlayerMiniGameTask::CallTaskCompleteCallback (0x81ec74). onDoorOpen -> cContainerWnd::AddBgWindow
//     (0x81e504, the opened-safe interior at x=0x70 y=0x96 w=0x154 h=0x1dd).
//   - The gamepad shortcut (ProcessController 0x81d420 / InteractiveSpriteSucceeded 0x81cf18) and the digit / stetho
//     sprites (ShowNumbers 0x81e708 digit sprite ids via GetDigitalNumberSpriteId, ProcessStethoScale's 8 bars) are
//     display; this module tracks the state they would draw (lockedDigits / stethoLevel) but draws nothing.
#include "safecracking.h"

namespace safecracking {

// ------------------------------------------------------------------------------------------------------------
// The dial / tolerance math. Each helper mirrors the binary's integer ops exactly (unsigned / signed widths kept)
// so the port rounds and wraps identically; the magic constants are the compiler's divide-by-10 reciprocals.
// ------------------------------------------------------------------------------------------------------------

// cSafeCrackingApp::OnRelease (0x81ed44) / Process (0x81d4f0): notch 0..9 from a 16-bit dial rotation.
// angle' = rot + 0x10000 + 0xccc (the +0xccc is the half-notch rounding = the sweet-spot tolerance); notch =
// round(angle' * 10 / 65536), wrapped to 0..9. 0x1999 = 65536/10, the per-notch rotation.
int SafeCracking::notchFromRotation(int angle) {
    int32_t rot = static_cast<int16_t>(angle);          // ldrsh, the sprite's stored int16 rotation
    uint32_t w8 = static_cast<uint32_t>(rot + 0x10000 + 0xccc);
    uint64_t prod = static_cast<uint64_t>(w8) * 0xa003c017ull;   // umull
    uint32_t raw = static_cast<uint32_t>(prod >> 44);
    uint32_t snapped = raw * 0x1999u;                   // SetSpriteRotation(notch * 0x1999)
    int64_t x9 = static_cast<int64_t>(static_cast<int32_t>(snapped)) * static_cast<int64_t>(static_cast<int32_t>(0xa00a00a1u));
    uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(x9) >> 32);
    uint32_t w9 = hi + snapped;                         // add w9, w9, w8 (w8 = snapped)
    int32_t w10 = static_cast<int32_t>(w9) >> 12;
    w9 = static_cast<uint32_t>(w10 + static_cast<int32_t>(w9 >> 31));
    int val = static_cast<int32_t>(w9);
    if (static_cast<int32_t>(snapped) > 0xffef) val -= 0xa;   // wrap the top half-notch back into 0..9
    return val;
}

// cSafeCrackingApp::UpdateStethoTarget (0x81eaf0): the wrapped notch distance cur->target in the turning direction.
// dir 0 -> the CCW rule ((cur - target) mod 10); else the CW rule ((target - cur) mod 10). Result 0..9.
int SafeCracking::wrappedDistance(int target, int cur, int dir) {
    int w10 = static_cast<int8_t>(target);              // ldrsb +0x1ca
    int w1 = cur;
    int w12 = w1 - w10;
    int w14 = w10 - w1;
    if (w10 > static_cast<int8_t>(w1)) w12 += 10;
    if (w10 < static_cast<int8_t>(w1)) w14 += 10;
    return (dir == 0) ? w12 : w14;
}

// cSafeCrackingApp::UpdateStethoTarget (0x81eaf0): the stetho meter target for a wrapped distance. The formula is
// 3 * (dist/10)^3 in 20.12 fixed point, taken as a byte; small distances read ~0 and only the exact notch (handled
// by the caller) forces the full 7. 0x66666667 is the divide-by-10 reciprocal.
int SafeCracking::stethoFromDistance(int dist) {
    int32_t d = static_cast<int32_t>(static_cast<int8_t>(dist)) * 4096;   // sbfiz dist << 12
    int64_t x8 = static_cast<int64_t>(d) * static_cast<int64_t>(0x66666667);
    uint32_t w9 = static_cast<uint32_t>(static_cast<uint64_t>(x8) >> 63);
    int64_t x8s = x8 >> 34;
    uint32_t w8 = static_cast<uint32_t>(static_cast<int64_t>(x8s) + static_cast<int64_t>(w9));   // round(d/10)
    uint32_t sq = static_cast<uint32_t>(
        static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(w8)) * static_cast<int32_t>(w8)) >> 12);
    uint32_t cube = sq * w8;
    uint32_t r = cube >> 12;
    r = r + (r << 1);                                   // * 3
    r = (r >> 11) & 0xFFFFF;
    return static_cast<int>(r & 0xFF);
}

// cSafeCrackingApp::GetDigitalNumberSpriteId (0x81ecc8): clamp n to 0..9 and map to the digit sprite id n + 0xb.
int SafeCracking::getDigitalNumberSpriteId(int n) {
    int c = n < 9 ? n : 9;
    if (c < 0) c = 0;
    return c + 0xb;
}

// ------------------------------------------------------------------------------------------------------------
// The puzzle state machine.
// ------------------------------------------------------------------------------------------------------------

void SafeCracking::reset() {
    step_ = kStepPreIntro;
    rotation_ = 0;
    combo_ = {{0, 0, 0}};
    locked_ = {{0, 0, 0}};
    target_ = 0;
    dirFlag_ = 0;
    armed_ = false;
    atTarget_ = false;
    stethoTarget_ = 0;
    stethoLevel_ = 0;
    lastNotch_ = -1;
    introTimer_ = 0;
    doorTimer_ = 0;
    doorOpen_ = false;
    finishCode_ = 0;
}

void SafeCracking::start(int target0, int target1, int target2) {
    reset();
    combo_ = {{target0, target1, target2}};
    step_ = kStepIntro;             // the frame slides in before the first tumbler
    introTimer_ = kIntroTotal;      // +0x2ac
}

// cSafeCrackingApp::FindWantedRots (0x81d33c): roll the three targets, keeping each far enough from the previous
// notch in the turning direction so a tumbler is not trivially next door. rng(n) returns [0, n).
std::array<int, 3> SafeCracking::findWantedRots(const std::function<uint32_t(uint32_t)>& rng, int dir) {
    combo_[0] = static_cast<int>(rng(7)) + 3;           // Rand32NonCritical(7) + 3, in [3, 9]
    int prev = combo_[0] & 0xff;
    for (int i = 1; i < 3; ++i) {
        int r;
        for (;;) {
            r = static_cast<int>(rng(10));              // Rand32NonCritical(10)
            bool accept;
            if (dir != 0) {                             // +0x1cb != 0 path (0x81d37c)
                if (r > prev + 3) accept = true;
                else { int gap = prev - r; accept = (gap > 0 && gap < 7); }
            } else {                                    // +0x1cb == 0 path (0x81d398)
                if (r + 3 < prev) accept = true;
                else accept = (prev < r && r < prev + 7);
            }
            if (accept) break;
        }
        combo_[i] = r;
        prev = r & 0xff;
    }
    return combo_;
}

// The Process step-2/3/4 first-entry setup: copy the active tumbler's target in and pick the turn direction.
void SafeCracking::armTumbler() {
    armed_ = true;                                      // +0x1b0 = 1
    int t = tumbler();                                  // 0..2
    target_ = combo_[t];                                // +0x1ca = wanted[t]
    dirFlag_ = (t == 1) ? 1 : 0;                        // CCW, CW, CCW -> flag 0, 1, 0
}

// cSafeCrackingApp::UpdateStethoTarget (0x81eaf0): recompute the stetho meter target from the live notch.
void SafeCracking::updateStethoTarget(int curNotch) {
    stethoTarget_ = stethoFromDistance(wrappedDistance(target_, curNotch, dirFlag_));
}

// cSafeCrackingApp::ProcessStethoScale (0x81eb94): step the displayed level one towards the target a frame.
void SafeCracking::processStethoScale() {
    if (stethoLevel_ < stethoTarget_) ++stethoLevel_;
    else if (stethoLevel_ > stethoTarget_) --stethoLevel_;
    else stethoTarget_ = 0;                             // settled: the target clears (b.ls strb wzr,[+0x1cf])
}

// The per-frame tumbler block shared by Process steps 2/3/4.
void SafeCracking::processTumbler() {
    if (!armed_) armTumbler();
    int notch = notchFromRotation(rotation_);
    atTarget_ = (notch == target_);                     // +0x1cc
    locked_[tumbler()] = notch;                         // ShowNumbers(notch): the live digit the display shows
    if (notch != lastNotch_) sfx(kSfxTick);             // 0x2e1 as the dial clicks past a notch
    updateStethoTarget(notch);
    if (atTarget_) { stethoTarget_ = kStethoMax; sfx(kSfxAtTarget); }   // 0x2e0, the sweet spot
    processStethoScale();
    lastNotch_ = notch;                                 // +0x2b2
}

// cSafeCrackingApp::NextStep (0x81d320): reset the per-step flags and advance +0x1ac.
void SafeCracking::nextStep() {
    armed_ = false;                                     // +0x1b0 = 0
    atTarget_ = false;                                  // +0x1cc = 0
    lastNotch_ = -1;                                    // +0x1a6 cleared; next notch re-ticks
    ++step_;                                            // +0x1ac + 1
}

// cSafeCrackingApp::SetFinished (0x81ec74): flag done and fire the player's minigame-complete callback.
void SafeCracking::setFinished() {
    finishCode_ = 1;                                    // +0x1a5 = 1
    if (onFinished) onFinished();
}

// cSafeCrackingApp::Process (0x81d4f0): one 30 Hz frame.
void SafeCracking::process() {
    switch (step_) {
    case kStepIntro:
        if (introTimer_ < 1) { step_ = kStepTumbler0; armed_ = false; atTarget_ = false; introTimer_ = 0; }
        else introTimer_ -= kIntroStep;                 // +0x2ac -= 0x16 (0x81d6d0)
        break;
    case kStepTumbler0:
    case kStepTumbler1:
    case kStepTumbler2:
        processTumbler();
        break;
    case kStepWinOutro:
        introTimer_ += kIntroStep;                      // +0x2ac += 0x16 (0x81dc10)
        if (introTimer_ > kIntroTotal - 1) { introTimer_ = 0; step_ = kStepWinSpin; doorTimer_ = 0; doorOpen_ = false; }
        break;
    case kStepWinSpin:
        // The dial spins the safe open (SetSpriteRotation(rot - 0x71c) a frame until rot < -0x38e3); that spin is
        // cosmetic, so the port opens the door on entry and then runs the faithful 45-frame wait to SetFinished.
        if (!doorOpen_) { doorOpen_ = true; sfx(kSfxDoorOpen); if (onDoorOpen) onDoorOpen(); }   // +0x1cd, sfx 0x2e5
        else if (++doorTimer_ >= kDoorOpenDelay) setFinished();   // +0x1a6 >= 0x2d
        break;
    default:
        break;
    }
}

// cSafeCrackingApp::OnRelease (0x81ed44): the player let go of the dial. Only the three tumbler steps act (the
// gate is (step - 2) <= 2 unsigned). On the exact target notch the tumbler locks and advances; otherwise it is a
// free retry (the original only resets the grip sprites).
void SafeCracking::onRelease() {
    if (step_ < kStepTumbler0 || step_ > kStepTumbler2) return;
    int notch = notchFromRotation(rotation_);
    atTarget_ = (notch == target_);                     // +0x1cc = (notch == +0x1ca)
    locked_[tumbler()] = notch;                         // ShowNumbers(notch)
    if (atTarget_) {
        sfx(kSfxLockIn);                                // 0x2e4
        nextStep();                                     // advance; the third tumbler advances into kStepWinOutro
        if (step_ == kStepWinOutro) introTimer_ = 0;
    }
}

}  // namespace safecracking
