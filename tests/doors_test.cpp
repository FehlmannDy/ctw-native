// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
// Exercise the cDynDoorProp open/close state machine and geometry, and the cDoorHandler enter-range trigger, with
// synthetic data only. Every expected value is re-derived from the libGame.so constants cited in doors.cpp/.h:
//   progress +0x4F6 counts 100 -> 0 by the move speed; state +0x4FE is 0 shut / 1 opening / 2 open / 3 closing /
//   4 shut-by-itself; the open hold triggers at progress 50 (0x32); the heading matrix uses SimpleMover::SetHeading;
//   cDoorHandler uses enter radius 0x2000 / leave radius 0x1000, and a 1-unit (0x1000) walk-in vicinity.
#include "doors.h"
#include "game.h"
#include <cstdint>
#include <cstdio>

namespace {
int failures;
void check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; } }
}

// Reads the private state of a DoorProp / DoorHandler (declared as friend in doors.h), the way player.h's test
// access does. No behaviour lives here: it only observes the fields the state machine drives.
struct DoorTestAccess {
    static uint8_t progress(const DoorProp& d) { return d.progress_; }
    static uint8_t state(const DoorProp& d) { return d.state_; }
    static uint8_t speed(const DoorProp& d) { return d.speed_; }
    static uint8_t hold(const DoorProp& d) { return d.hold_; }
    static bool autoClose(const DoorProp& d) { return d.autoClose_; }
};

int main() {
    using A = DoorTestAccess;
    const int32_t origin[3] = {0, 0, 0};

    // ---- geometry: SimpleMover::SetHeading(0) and Entity::GetOffsetInWorldCoords -------------------------------
    // heading 0 -> forward = (0, 4096, 0), right = (fwd.y, -fwd.x, 0) = (4096, 0, 0). (observation: the port halves
    // the 20.12 width/height into cFixed<4,12>; the matrix rows are exact integers at heading 0.)
    {
        const int32_t pos[3] = {1000, 2000, 3000};
        DoorProp d(pos, 0, 0x2800, 0x4000, 0);
        check(d.fwd[0] == 0 && d.fwd[1] == 4096 && d.fwd[2] == 0, "heading 0 forward is +y");
        check(d.right[0] == 4096 && d.right[1] == 0 && d.right[2] == 0, "heading 0 right is +x");
        int32_t out[3];
        const int32_t front[3] = {0, 0x2000, 0};   // kFront (cDoorHandler approach point when entering)
        d.offsetInWorld(front, out);
        check(out[0] == 1000 && out[1] == 2000 + 0x2000 && out[2] == 3000, "kFront offset is 2 units along +y");
        const int32_t side[3] = {0x1800, 0, 0};     // 1.5 units to the door's right
        d.offsetInWorld(side, out);
        check(out[0] == 1000 + 0x1800 && out[1] == 2000 && out[2] == 3000, "a right-hand offset moves along +x");
    }

    // ---- opening interpolation: hold 0, not auto-closing, move speed 2 ----------------------------------------
    // progress steps down by the speed each frame while opening; at 50 (0x32) a non-auto door with no hold latches
    // open (state 2) and stops moving. 100 -> 50 at speed 2 is 25 frames.
    {
        DoorProp d(origin, 0, 0x2800, 0x4000, 0);
        d.setSpeed(2);
        d.open(false);
        check(A::state(d) == 1 && A::progress(d) == 100, "open() enters state 1 opening at progress 100");
        for (int i = 0; i < 25; ++i) d.process();
        check(A::progress(d) == 50 && A::state(d) == 1, "25 frames at speed 2 reach progress 50, still opening");
        d.process();
        check(A::state(d) == 2 && A::progress(d) == 50, "the frame that sees progress 50 latches open (state 2)");
        d.process();
        check(A::state(d) == 2 && A::progress(d) == 50, "an open held door does not move");

        // ---- closing: state 3 steps down to 0, then a shut door resets to state 0 / progress 100 --------------
        d.close(false);
        check(A::state(d) == 3 && A::progress(d) == 50, "close() from open enters state 3 at the same progress");
        for (int i = 0; i < 25; ++i) d.process();   // 50 -> 0 at speed 2
        check(A::progress(d) == 0 && A::state(d) == 3, "25 frames at speed 2 close it to progress 0");
        d.process();
        check(A::state(d) == 0 && A::progress(d) == 100, "a closed non-auto door resets to shut (state 0, 100)");
    }

    // ---- auto-close with no hold: progress runs all the way to 0, then the door shuts itself (state 4) ----------
    {
        DoorProp d(origin, 0, 0x2800, 0x4000, 0);   // speed 1 by default
        d.open(true);
        check(A::autoClose(d) && A::state(d) == 1, "open(true) sets the auto-close flag");
        int frames = 0;
        while (A::state(d) != 4 && frames < 500) { d.process(); ++frames; }
        check(A::state(d) == 4 && A::progress(d) == 0, "an auto, hold-0 door ends shut-by-itself (state 4) at 0");
        check(frames == 101, "100 frames of speed-1 travel, then one frame at 0 flips to state 4");
    }

    // ---- hold of 1 + auto-close: the latch frame goes straight to closing (state 3), then to state 4 -----------
    {
        DoorProp d(origin, 0, 0x2800, 0x4000, 1);   // hold 1, speed 1
        d.open(true);
        for (int i = 0; i < 50; ++i) d.process();   // 100 -> 50 opening
        check(A::progress(d) == 50 && A::state(d) == 1, "50 speed-1 frames bring a hold door to the latch point");
        d.process();
        check(A::state(d) == 3 && A::hold(d) == 1, "hold 1 + auto-close turns the latch frame into closing (state 3)");
        int frames = 0;
        while (A::state(d) != 4 && frames < 500) { d.process(); ++frames; }
        check(A::state(d) == 4, "the auto door then closes to shut-by-itself (state 4)");
    }

    // ---- reversal: turning round mid-swing reflects progress about 100 (cDynDoorProp::Open/Close) ---------------
    {
        DoorProp d(origin, 0, 0x2800, 0x4000, 0);   // speed 1
        d.open(false);
        for (int i = 0; i < 10; ++i) d.process();   // opening -> progress 90, state 1
        check(A::progress(d) == 90 && A::state(d) == 1, "ten opening frames leave progress 90");
        d.close(false);
        check(A::state(d) == 3 && A::progress(d) == 10, "closing from opening reflects 90 to 100-90 = 10");
        d.open(false);
        check(A::state(d) == 1 && A::progress(d) == 90, "reopening from closing reflects 10 back to 90");
    }

    // ---- cDoorHandler enter trigger: DoMainStuff / WhenEntersVicinityOf / DoTurn ------------------------------
    // Door at the origin, heading 0, so the entering approach point kFront is (0, 0x2000, 0). The player starts
    // walking (scriptWalk.on true); DoTurn (which sets scriptWalk.on false) fires once he is within 1 unit (0x1000)
    // of that point, or immediately if he begins within the 2-unit (0x2000) enter circle.
    {
        Game g;
        g.player.pos[0] = 0;
        g.player.pos[1] = 0x2000 + 0x4000;   // 4 units beyond the approach point: outside the 2-unit circle
        g.player.pos[2] = 0;
        DoorHandler h;
        h.start(g, origin, 0, DoorHandler::kEnter, [] {}, false);
        check(h.active() && g.scriptWalk.on, "starting far from the door walks the player in (no turn yet)");
        h.tick(g);
        check(g.scriptWalk.on, "a frame still 4 units away does not trigger the turn");
        g.player.pos[1] = 0x2000 + 0x800;    // 0.5 units from the approach point: inside the 1-unit vicinity
        h.tick(g);
        check(!g.scriptWalk.on, "entering the 1-unit vicinity triggers DoTurn (stops the walk)");
    }
    {
        Game g;
        g.player.pos[0] = 0;
        g.player.pos[1] = 0x2000;   // exactly on the approach point: inside the enter circle at once
        g.player.pos[2] = 0;
        DoorHandler h;
        h.start(g, origin, 0, DoorHandler::kEnter, [] {}, false);
        check(h.active() && !g.scriptWalk.on, "starting inside the enter circle turns immediately (no walk-in)");
    }

    if (failures) { std::fprintf(stderr, "%d door check(s) failed\n", failures); return 1; }
    std::printf("all door checks passed\n");
    return 0;
}
