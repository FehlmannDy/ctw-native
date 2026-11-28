// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Gui::cDrugVanSlashApp swipe + state logic, checked against libGame.so (addresses in drugvanslash.h):
//   * four slashes along the door polyline waypoints (table 0x485118);
//   * a sweep completes a slash when it runs forward along the segment far enough and close enough
//     (cSlash::Update one-way completion, 0x7f38b4); too-short / reversed / off-line sweeps are rejected;
//   * the fourth slash fires the cut-open sfx 0x145 and enters TearingOpen (Process 0x7f4b8c/0x7f4c08);
//   * the two-panel 10-frame tear (+0x19c -> 2, +0x1c0 = 10) then grabs (or auto-awards for van kind 4);
//   * FinishCode() is a constant 1 and there is no timer / fail path (Init/Reset/Process have none).
// Synthetic data only; no rendering/sound/inventory (those are hooks).
#include "drugvanslash.h"

#include <cmath>
#include <cstdio>

using namespace drugvanslash;

static int failures;
static void check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; } }

// Re-derive a segment's geometry straight from the waypoint table so expectations track the binary constants.
static Vec2 wp(int i) { return kWaypoints[i]; }
static float segLen(int i) {
    const Vec2 a = wp(i), b = wp(i + 1);
    return std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
}

int main() {
    // --- a clean forward sweep along slash 0 registers ---
    {
        DrugVanSlash g;
        g.start(VanKind::Drugs, 7, 3);
        check(g.active(), "start -> active");
        check(g.state() == State::Slashing, "start -> Slashing");
        check(g.slashesNeeded() == 4, "four slashes needed");
        check(g.slashesDone() == 0, "no slashes done at start");
        check(g.finishCode() == 1, "FinishCode is constant 1");

        const bool ok = g.swipe(wp(0), wp(1));   // (700,369) -> (700,88), the full first segment
        check(ok, "full forward sweep completes slash 0");
        check(g.slashesDone() == 1, "slash index advanced to 1");
        check(g.state() == State::Slashing, "still slashing after one slash");
    }

    // --- a too-short sweep is rejected (covers < 0.85 of the segment length) ---
    {
        DrugVanSlash g;
        g.start();
        const Vec2 a = wp(0);
        const float L = segLen(0);                       // 281
        // move only half the needed forward distance along the segment (downward -> up is -y here)
        const float d = 0.5f * g.tolerance.minAlongFrac * L;
        const bool ok = g.swipe(a, Vec2{a.x, a.y - d});
        check(!ok, "too-short sweep rejected");
        check(g.slashesDone() == 0, "index unchanged after too-short sweep");
    }

    // --- a reversed sweep (correct line, wrong way) is rejected ---
    {
        DrugVanSlash g;
        g.start();
        const bool ok = g.swipe(wp(1), wp(0));   // backward along segment 0
        check(!ok, "reversed sweep rejected");
        check(g.slashesDone() == 0, "index unchanged after reversed sweep");
    }

    // --- a sweep parallel to the segment but too far off the door line is rejected ---
    {
        DrugVanSlash g;
        g.start();
        const float off = g.tolerance.maxPerp + 20.f;    // beyond the perpendicular tolerance
        const Vec2 a = wp(0), b = wp(1);
        const bool ok = g.swipe(Vec2{a.x + off, a.y}, Vec2{b.x + off, b.y});
        check(!ok, "off-line sweep rejected");
        check(g.slashesDone() == 0, "index unchanged after off-line sweep");
    }

    // --- a sweep starting too far before the segment start is rejected ---
    {
        DrugVanSlash g;
        g.start();
        const Vec2 a = wp(0), b = wp(1);
        // start maxStartGap+40 further "down" (+y) so startAlong is more negative than -maxStartGap
        const float back = g.tolerance.maxStartGap + 40.f;
        const bool ok = g.swipe(Vec2{a.x, a.y + back}, b);
        check(!ok, "sweep starting too far before segment rejected");
        check(g.slashesDone() == 0, "index unchanged after early-start sweep");
    }

    // --- completing all four slashes cuts the van open: sfx + TearingOpen ---
    {
        DrugVanSlash g;
        int sfxSample = 0, sfxVol = -1;
        g.playSfx = [&](int s, int v) { sfxSample = s; sfxVol = v; };
        g.start(VanKind::Drugs, 11, 5);

        for (int i = 0; i < kSlashCount; ++i) {
            const bool ok = g.swipe(wp(i), wp(i + 1));
            check(ok, "each in-order slash registers");
            check(g.slashesDone() == i + 1, "slash index tracks the count");
        }
        check(g.state() == State::TearingOpen, "four slashes -> TearingOpen");
        check(sfxSample == kCutOpenSfx, "cut-open sfx 0x145 played on the fourth slash");
        check(sfxVol == 0x7f, "cut-open sfx at full volume 0x7f");

        // a swipe after the van is open does nothing
        check(!g.swipe(wp(0), wp(1)), "swipe ignored once not slashing");
    }

    // --- tear animation: two panels of ten frames, then the grab, then the award/result/finish ---
    {
        DrugVanSlash g;
        bool awarded = false;
        VanKind awKind = VanKind::Drugs;
        int awItem = -1, awQty = -1;
        g.awardStash = [&](VanKind k, int id, int q) { awarded = true; awKind = k; awItem = id; awQty = q; };
        bool doneCalled = false, doneSuccess = false;
        g.start(VanKind::Drugs, 13, 9, [&](bool s) { doneCalled = true; doneSuccess = s; });

        for (int i = 0; i < kSlashCount; ++i) g.swipe(wp(i), wp(i + 1));
        check(g.state() == State::TearingOpen, "tear: starts in TearingOpen");

        // mid-first-panel progress is between 0 and 1
        g.process();
        check(g.tearProgress() > 0.f && g.tearProgress() < 1.f, "tear progress reported mid-panel");

        // finish the two panels (we already pumped 1 of the 2*10 frames)
        for (int f = 1; f < kTearPanels * kTearPanelFrames; ++f) g.process();
        check(g.state() == State::Grabbing, "two full tear panels -> Grabbing (Drugs van, not auto-award)");
        check(!awarded, "nothing awarded before the grab");

        g.grab();
        check(g.state() == State::Result, "grab -> Result");
        check(awarded, "grab awards the stash");
        check(awKind == VanKind::Drugs && awItem == 13 && awQty == 9, "award carries van kind / item / qty");

        check(!g.isFinished(), "not finished until the result frame runs");
        g.process();
        check(g.isFinished(), "Result frame marks finished (+0x190)");
        check(doneCalled && doneSuccess, "done(true) fired on finish (FinishCode 1, always success)");
    }

    // --- van kind 4 auto-awards after the tear and skips the grab ---
    {
        DrugVanSlash g;
        bool awarded = false;
        g.awardStash = [&](VanKind, int, int) { awarded = true; };
        g.start(VanKind::Special4, 1, 1);
        for (int i = 0; i < kSlashCount; ++i) g.swipe(wp(i), wp(i + 1));
        for (int f = 0; f < kTearPanels * kTearPanelFrames; ++f) g.process();
        check(g.state() == State::Result, "Special4 van skips Grabbing and goes to Result");
        check(awarded, "Special4 van auto-awards after the tear");
    }

    if (failures == 0) std::printf("drugvanslash: all checks passed\n");
    return failures ? 1 : 0;
}
