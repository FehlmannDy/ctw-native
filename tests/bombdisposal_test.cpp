// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Gui::cBombDisposalApp puzzle state machine + validation math, checked against libGame.so (see bombdisposal.h).
//   - 4 faceplate screws (Process i<4 loop 0x007e8528) must be turned before the wire cutter is enabled
//     (ProcessController gate +0x1b4 == 3, 0x007e7e20).
//   - 3 wires (SelectWire 0x007e7f6c); the correct wire = Rand32NonCritical(3) -> +0x204 (Init 0x007e6b74).
//   - SelectWireCut (0x007e8040): success byte +0x20f = (cut == correct); EITHER outcome finishes once (+0x210).
//   - No timer / no penalty exist in the 27 cBombDisposalApp routines: losing == cutting the wrong wire, not a
//     timeout. This test therefore exercises the real loss condition (wrong wire) in place of a nonexistent clock.
// Synthetic data only; no GL/audio context (playSfx is captured to assert the cut/screw SFX fire).
#include "bombdisposal.h"
#include <cstdio>
#include <vector>

struct BombDisposalTestAccess {
    static void forceCutting(BombDisposal& b) {
        // Drive the phase machine exactly as the game does: four screw turns then two reveal frames.
        for (int i = 0; i < BombDisposal::kScrews; ++i) b.turnScrew();
        b.processReveal();  // 1 -> 2
        b.processReveal();  // 2 -> 3
    }
};

static int failures;
static void check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; } }

int main() {
    using TA = BombDisposalTestAccess;

    // --- phase 1: four screws then reveal; cutting is locked until +0x1b4 == 3 ---
    {
        BombDisposal b;
        b.start([] { return 1; });
        check(b.active(), "start activates");
        check(b.phase() == BombDisposal::kUnscrewing, "starts unscrewing (+0x1b4==0)");
        check(b.correctWire() == 1, "correct wire = rand3() = 1 (+0x204)");
        check(!b.cutting(), "cutting locked before screws out");

        // The first three turns pop a screw but do NOT reveal; the fourth pops the last and begins the reveal.
        check(!b.turnScrew(), "screw 1 does not finish phase");
        check(!b.turnScrew(), "screw 2 does not finish phase");
        check(!b.turnScrew(), "screw 3 does not finish phase");
        check(b.screwsOut() == 3, "three screws out");
        check(b.turnScrew(), "screw 4 pops the last screw -> reveal");
        check(b.screwsOut() == BombDisposal::kScrews, "all four screws out");
        check(b.phase() == BombDisposal::kRevealing1, "reveal step 1 (+0x1b4==1)");
        check(!b.turnScrew(), "no more screws to turn");
        check(!b.cutting(), "still locked during reveal step 1");

        b.processReveal();
        check(b.phase() == BombDisposal::kRevealing2, "reveal step 2 (+0x1b4==2)");
        check(!b.cutting(), "still locked during reveal step 2");
        b.processReveal();
        check(b.phase() == BombDisposal::kCutting, "cutting enabled (+0x1b4==3)");
        check(b.cutting(), "cutting now allowed");
    }

    // --- screw SFX hook fires 0x189 once per screw ---
    {
        BombDisposal b;
        std::vector<int> sfx;
        b.playSfx = [&](int s, int) { sfx.push_back(s); };
        b.start([] { return 0; });
        for (int i = 0; i < BombDisposal::kScrews; ++i) b.turnScrew();
        check((int)sfx.size() == BombDisposal::kScrews, "one screw-pop SFX per screw");
        bool allPop = true;
        for (int s : sfx) allPop = allPop && (s == BombDisposal::kSfxScrewPop);
        check(allPop, "screw SFX id == 0x189");
    }

    // --- cutting before the wire phase is a no-op (cannot end the game early) ---
    {
        BombDisposal b;
        bool called = false;
        b.start([] { return 2; }, [&](bool) { called = true; });
        check(!b.selectWireCut(0), "selectWireCut no-op while unscrewing");
        check(!b.completed(), "no completion before wire phase");
        check(!called, "done callback not fired early");
    }

    // --- selection clamp 0..2 (ProcessController left if sel>=1, right if sel<=1) ---
    {
        BombDisposal b;
        b.start([] { return 0; });
        TA::forceCutting(b);
        check(b.selected() == 0, "selection starts at 0");
        b.moveSelection(-1);
        check(b.selected() == 0, "cannot move left past 0");
        b.moveSelection(1);
        check(b.selected() == 1, "move right to 1");
        b.moveSelection(1);
        check(b.selected() == 2, "move right to 2");
        b.moveSelection(1);
        check(b.selected() == 2, "cannot move right past 2 (3 wires)");
        b.moveSelection(-1);
        check(b.selected() == 1, "move left to 1");
    }

    // --- CORRECT wire cut wins; completes once; success flag set ---
    {
        BombDisposal b;
        std::vector<int> sfx;
        int wins = 0; bool result = false;
        b.playSfx = [&](int s, int) { sfx.push_back(s); };
        b.start([] { return 2; }, [&](bool ok) { ++wins; result = ok; });
        TA::forceCutting(b);
        b.setSelected(2);                 // the correct wire (+0x204 == 2)
        check(b.cutSelected(), "cutting the correct wire returns success");
        check(b.success(), "success flag (+0x20f) set on correct cut");
        check(b.completed(), "completion callback fired (+0x210)");
        check(b.isFinished(), "IsFinished (+0x20d) set after completion");
        check(wins == 1 && result == true, "done(true) fired exactly once");
        bool sawCut = false;
        for (int s : sfx) sawCut = sawCut || (s == BombDisposal::kSfxWireCut);
        check(sawCut, "wire-cut SFX id == 0x152 played");

        // One-shot guard: a second cut must not re-fire the callback.
        check(!b.cutSelected() || wins == 1, "second cut does not re-complete");
        check(wins == 1, "done fired only once (guard +0x210)");
    }

    // --- WRONG wire cut loses (ends immediately, no timer, no penalty) ---
    {
        BombDisposal b;
        int calls = 0; bool result = true;
        b.start([] { return 0; }, [&](bool ok) { ++calls; result = ok; });
        TA::forceCutting(b);
        b.setSelected(2);                 // wrong wire (correct is 0)
        check(!b.cutSelected(), "cutting the wrong wire returns failure");
        check(!b.success(), "success flag (+0x20f) clear on wrong cut");
        check(b.completed(), "wrong cut still ends the game (callback fired)");
        check(calls == 1 && result == false, "done(false) fired exactly once on loss");
    }

    // --- SetFinished / EndApp flag semantics ---
    {
        BombDisposal b;
        int calls = 0; bool res = false;
        b.start([] { return 1; }, [&](bool ok) { ++calls; res = ok; });
        check(b.setFinished(true), "SetFinished(true) completes the game");
        check(calls == 1 && res == true, "SetFinished routes done(true) once");
        check(!b.setFinished(false), "SetFinished guarded (+0x210): no second completion");
        check(calls == 1, "done still fired only once");
    }
    {
        BombDisposal b;
        b.start([] { return 0; });
        check(!b.isFinished(), "IsFinished clear at start");
        check(b.finishCode() == 0, "FinishCode 0 at start");
        b.endApp();                       // EndApp: strh 0x0101 at +0x20d
        check(b.isFinished(), "EndApp sets IsFinished (+0x20d) = 1");
        check(b.finishCode() == 1, "EndApp sets FinishCode (+0x20e) = 1");
    }

    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("all bomb-disposal checks passed\n");
    return 0;
}
