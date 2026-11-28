// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
// Exercises the cMapApp PDA adapter: the cScrollMomentum fling maths and the drag -> fling gesture wiring. No
// rendering and no Game are needed (the pan is a no-op on an unbuilt map, but the gesture state is still driven).
#include "pdamap.h"
#include <cstdio>

static int failures;
static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

// Reach into MapPdaApp's drag/momentum state.
struct MapPdaTestAccess {
    static MapPanMomentum& momentum(MapPdaApp& a) { return a.momentum_; }
    static bool dragging(const MapPdaApp& a) { return a.dragging_; }
    static int32_t lastDx(const MapPdaApp& a) { return a.lastDxQ12_; }
};

int main() {
    using pda::Touch;

    // --- cScrollMomentum::Process: 0.75 decay, stop below 1.0 px -----------------
    {
        MapPanMomentum m;
        m.applyForce(100 << 12, 0);                 // 100 px/frame to the right (Q12)
        check(m.active(), "applyForce makes the momentum active");
        check(m.process() && m.vxQ12() == ((100 << 12) * 3 >> 2), "one step decays by 0.75");
        // 100 -> 75 -> 56.25 ... keep stepping until it falls below 1.0 px and snaps to 0.
        int steps = 1;
        while (m.process()) { ++steps; check(steps < 100, "momentum terminates"); }
        check(m.vxQ12() == 0 && m.vyQ12() == 0 && !m.active(), "momentum snaps to rest");
    }

    // --- a sub-pixel force stops immediately (magnitude < 4096) ------------------
    {
        MapPanMomentum m;
        m.applyForce(2048, 2048);                   // |v| = sqrt(2) * 0.5 px < 1 px
        check(!m.process() && !m.active(), "a sub-pixel fling stops at once");
    }

    // --- negative decay keeps the sign (binary's (v*3)>>2) -----------------------
    {
        MapPanMomentum m;
        m.applyForce(-(40 << 12), 0);
        m.process();
        check(m.vxQ12() == (int32_t)(((int64_t)(-(40 << 12)) * 3) >> 2) && m.vxQ12() < 0,
              "negative velocity decays and stays negative");
    }

    // --- stop() zeroes an active fling ------------------------------------------
    {
        MapPanMomentum m;
        m.applyForce(10 << 12, 10 << 12);
        m.stop();
        check(!m.active() && m.vxQ12() == 0 && m.vyQ12() == 0, "stop() clears the velocity");
    }

    // --- drag gesture: press -> hold -> release seeds the fling ------------------
    {
        MapPdaApp app;                               // no map/Game: pan is a no-op, gesture still drives momentum
        Touch t;
        t.down = true;  t.x = 100; t.y = 100;        // press (DragStarted)
        app.onInput(t);
        check(MapPdaTestAccess::dragging(app), "press starts a drag");

        t.x = 130; t.y = 90;                         // held (IsBeingDragged): delta (+30, -10)
        app.onInput(t);
        check(MapPdaTestAccess::lastDx(app) == (30 << 12), "drag records the last delta in Q12");

        t.down = false;                              // release (DragEnded): seeds the fling
        app.onInput(t);
        check(!MapPdaTestAccess::dragging(app), "release ends the drag");
        check(MapPdaTestAccess::momentum(app).active(), "release seeds the fling");
        check(MapPdaTestAccess::momentum(app).vxQ12() == (30 << 12), "fling velocity = last drag delta");

        // onProcess steps the fling once the drag is over.
        const int32_t before = MapPdaTestAccess::momentum(app).vxQ12();
        app.onProcess();
        check(MapPdaTestAccess::momentum(app).vxQ12() < before, "onProcess decays the fling");
    }

    // --- onAppear fades in over ~10 frames before Running -----------------------
    {
        MapPdaApp app;
        int frames = 0;
        while (!app.onAppear()) { ++frames; check(frames < 20, "intro fade terminates"); }
        check(frames >= 9 && frames <= 10, "intro settles into Running after ~10 frames");
    }

    if (failures == 0) std::printf("pdamap_test: all checks passed\n");
    return failures ? 1 : 0;
}
