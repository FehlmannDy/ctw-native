// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// cSafeCrackingApp puzzle tests, synthetic data only (no engine, no GL). Every asserted value is re-derived from
// the binary's own constants:
//   - 10 notches, one notch = 0x1999 (65536/10) of a 16-bit rotation; half-notch rounding 0xccc = the tolerance.
//   - stetho meter 3*(dist/10)^3 scaled to a byte, forced to 7 at the exact notch.
//   - digit sprite id = clamp(n,0..9) + 0xb.
//   - 3 tumblers; a correct release advances; the third opens the safe after a 45-frame wait.
#include "safecracking.h"
#include <cstdio>

using namespace safecracking;

static int failures;
static void check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; }
}

// A notch's exact centre rotation (what SetSpriteRotation(notch * 0x1999) stores), as a signed 16-bit value.
static int notchRotation(int n) { return static_cast<int16_t>((n * kNotchStep) & 0xffff); }

static void testDialMath() {
    // Each notch centre reads back as that notch, 0..9 (the /10 round-trip).
    for (int n = 0; n < kNotches; ++n)
        check(SafeCracking::notchFromRotation(notchRotation(n)) == n, "notch centre round-trip");

    // Tolerance around notch 3 (centre 3*0x1999 = 0x4ccb). Within a half-notch (0xccc = 3276) still reads 3; a
    // clear half-notch past the centre reads the neighbour. Observed from the binary's rounding (+0xccc).
    int c = 3 * kNotchStep;
    check(SafeCracking::notchFromRotation(c) == 3, "at target reads 3");
    check(SafeCracking::notchFromRotation(c - 2000) == 3, "within tolerance low still 3");
    check(SafeCracking::notchFromRotation(c + 2000) == 3, "within tolerance high still 3");
    check(SafeCracking::notchFromRotation(c - 4000) == 2, "a notch below reads 2");
    check(SafeCracking::notchFromRotation(c + 4000) == 4, "a notch above reads 4");
    check(SafeCracking::notchFromRotation(c - kNotchRound) == 3, "minus half-notch rounds in");

    // Digit sprite id: clamp(0..9) + 0xb.
    check(SafeCracking::getDigitalNumberSpriteId(0) == 0xb, "digit 0 -> 0xb");
    check(SafeCracking::getDigitalNumberSpriteId(9) == 0x14, "digit 9 -> 0x14");
    check(SafeCracking::getDigitalNumberSpriteId(12) == 0x14, "digit clamps high to 9");
    check(SafeCracking::getDigitalNumberSpriteId(-3) == 0xb, "digit clamps low to 0");
}

static void testStetho() {
    // Wrapped distance, CCW rule (dir 0 = (cur - target) mod 10).
    check(SafeCracking::wrappedDistance(3, 3, 0) == 0, "dist at target is 0");
    check(SafeCracking::wrappedDistance(3, 5, 0) == 2, "dist cur ahead by 2");
    check(SafeCracking::wrappedDistance(3, 1, 0) == 8, "dist cur behind wraps to 8");
    // CW rule (dir 1 = (target - cur) mod 10): the mirror of the above.
    check(SafeCracking::wrappedDistance(3, 1, 1) == 2, "cw dist cur behind by 2");
    check(SafeCracking::wrappedDistance(3, 5, 1) == 8, "cw dist cur ahead wraps to 8");

    // The stetho cube 3*(dist/10)^3 scaled to a byte: flat near, rising only far out (the exact notch is forced to
    // 7 by the caller, tested in the state machine below).
    const int expect[10] = {0, 0, 0, 0, 0, 0, 1, 2, 3, 4};
    for (int d = 0; d < 10; ++d)
        check(SafeCracking::stethoFromDistance(d) == expect[d], "stetho curve value");
}

// Run the app forward one step at a time until it leaves the given step (bounded so a stuck machine fails loudly).
static void runUntilLeaves(SafeCracking& s, int step, int cap) {
    for (int i = 0; i < cap && s.step() == step; ++i) s.process();
}

static void testNearAndAtTarget() {
    int sfxAtTarget = 0, sfxTick = 0;
    SafeCracking s;
    s.playSfx = [&](int sample, int) { if (sample == kSfxAtTarget) ++sfxAtTarget; if (sample == kSfxTick) ++sfxTick; };
    s.start(3, 5, 7);
    runUntilLeaves(s, kStepIntro, 64);
    check(s.step() == kStepTumbler0, "intro reaches the first tumbler");

    // Park on the approach side, two notches along (CCW distance 2): the sweet-spot flag stays clear and the
    // stetho reads quiet (the cube is 0 for distances up to 5).
    s.setRotation(notchRotation(5));
    s.process();
    check(s.target() == 3, "first tumbler target armed from the combination");
    check(s.currentNotch() == 5, "dial sits on notch 5");
    check(!s.atTarget(), "off the target notch the sweet-spot flag is clear");
    check(SafeCracking::wrappedDistance(3, 5, 0) == 2, "notch 5 is a CCW distance of 2 from target 3");
    check(s.stethoTarget() == 0, "stetho stays quiet on the near approach side");
    int atBefore = sfxAtTarget;

    // Rotate onto the exact target notch: the flag sets, the stetho is forced to the max and the sweet-spot sound
    // fires. Moving across a notch also ticks.
    s.setRotation(notchRotation(3));
    s.process();
    check(s.atTarget(), "on the target notch the sweet-spot flag sets");
    check(s.stethoTarget() == kStethoMax, "stetho forced to 7 at the target");
    check(sfxAtTarget > atBefore, "sweet-spot sound fires at the target");
    check(sfxTick > 0, "the dial ticks as it passes notches");
}

static void testWrongAndLockIn() {
    SafeCracking s;
    s.start(3, 5, 7);
    runUntilLeaves(s, kStepIntro, 64);

    // A release on the wrong notch is a free retry: the step does not advance and nothing finishes.
    s.setRotation(notchRotation(6));
    s.process();
    check(!s.atTarget(), "wrong notch not at target");
    s.onRelease();
    check(s.step() == kStepTumbler0, "wrong release stays on the same tumbler");
    check(!s.finished(), "wrong release does not finish");

    // A release on the target notch locks it in and advances to the next tumbler.
    s.setRotation(notchRotation(3));
    s.process();
    s.onRelease();
    check(s.step() == kStepTumbler1, "correct release advances to the second tumbler");
    check(s.lockedDigits()[0] == 3, "first digit locked in as 3");
}

static void testFullCombinationWin() {
    int lockIns = 0, doorOpens = 0;
    bool finishedCb = false;
    SafeCracking s;
    s.playSfx = [&](int sample, int) { if (sample == kSfxLockIn) ++lockIns; if (sample == kSfxDoorOpen) ++doorOpens; };
    s.onDoorOpen = [&]() { /* orchestrator adds the opened-safe window here */ };
    s.onFinished = [&]() { finishedCb = true; };
    s.start(3, 5, 7);
    runUntilLeaves(s, kStepIntro, 64);

    const int combo[3] = {3, 5, 7};
    for (int t = 0; t < kTumblers; ++t) {
        check(s.step() == kStepTumbler0 + t, "on the expected tumbler");
        s.setRotation(notchRotation(combo[t]));
        s.process();
        check(s.target() == combo[t], "tumbler target matches the combination");
        s.onRelease();
    }
    check(lockIns == 3, "three lock-in sounds for three tumblers");
    check(s.step() == kStepWinOutro, "the third lock-in starts the win outro");

    runUntilLeaves(s, kStepWinOutro, 64);
    check(s.step() == kStepWinSpin, "the outro reaches the door-open spin");

    // The door opens once, then SetFinished fires after the 45-frame wait.
    check(!s.finished(), "not finished before the door-open wait elapses");
    for (int i = 0; i < kDoorOpenDelay + 4 && !s.finished(); ++i) s.process();
    check(doorOpens == 1, "the safe door opens exactly once");
    check(s.finished(), "the safe is cracked after the 45-frame wait");
    check(s.finishCode() == 1, "finish code is 1");
    check(finishedCb, "the minigame-complete callback fired");
    check(s.lockedDigits()[0] == 3 && s.lockedDigits()[1] == 5 && s.lockedDigits()[2] == 7,
          "the three locked digits are the combination");
}

static void testFindWantedRots() {
    // A deterministic rng cycling 0..power through values; verify the roll stays in range and keeps a gap from the
    // previous notch under the CCW rule (dir 0 rejects prev-3..prev).
    int idx = 0;
    const int seq[] = {4, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1, 2};
    auto rng = [&](uint32_t n) -> uint32_t { return static_cast<uint32_t>(seq[idx++ % 14]) % (n ? n : 1); };
    SafeCracking s;
    auto combo = s.findWantedRots(rng, 0);
    check(combo[0] >= 3 && combo[0] <= 9, "target0 in [3,9]");
    for (int i = 0; i < 3; ++i) check(combo[i] >= 0 && combo[i] <= 9, "every target in 0..9");
    // Under dir 0 a target must not sit in prev-3..prev (inclusive); verify the realised gaps obey it.
    for (int i = 1; i < 3; ++i) {
        int prev = combo[i - 1] & 0xff, r = combo[i];
        bool ok = (r + 3 < prev) || (prev < r && r < prev + 7);
        check(ok, "consecutive targets keep the separation rule");
    }
}

int main() {
    testDialMath();
    testStetho();
    testNearAndAtTarget();
    testWrongAndLockIn();
    testFullCombinationWin();
    testFindWantedRots();
    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("all safe-cracking checks passed\n");
    return 0;
}
