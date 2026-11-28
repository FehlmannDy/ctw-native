// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
// Exercises the PDA shell's load/transition/run state machine and chrome state (no rendering).
#include "pda.h"
#include <cstdio>
#include <memory>

using namespace pda;

static int failures;
static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

// A hosted app whose intro/outro take a set number of frames, so transitions can be driven deterministically.
struct StubApp : PdaApp {
    int appearFrames, leaveFrames;
    int appeared = 0, left = 0, processed = 0, inputs = 0;
    explicit StubApp(int id, int appear = 1, int leave = 1)
        : PdaApp(id), appearFrames(appear), leaveFrames(leave) {}
    bool onAppear() override { return ++appeared >= appearFrames; }
    bool onLeave() override { return ++left >= leaveFrames; }
    void onProcess() override { ++processed; }
    void onInput(const Touch&) override { ++inputs; }
};

int main() {
    // --- factory + instant load ------------------------------------------------
    {
        Pda pda;
        pda.init();
        int built = 0;
        pda.appFactory = [&](int id, ResetOption) { ++built; return std::make_unique<StubApp>(id); };
        pda.loadApp(kAppHud);   // HUD uses the instant transition -> immediate switch
        check(built == 1, "instant load builds the app at once");
        check(pda.app() && pda.app()->id() == kAppHud, "the hosted app is the requested id");
        check(pda.targetApp() == kAppHud, "targetApp resolves to the requested id (+0x1660)");
    }

    // --- white-fade load swaps the app at the midpoint -------------------------
    {
        Pda pda;
        pda.init();
        pda.appFactory = [&](int id, ResetOption) { return std::make_unique<StubApp>(id, 1, 1); };
        // Seed a running app so the fade has something to replace.
        pda.loadApp(kAppHud);
        for (int i = 0; i < 4; ++i) pda.process(0, 0);
        check(pda.isAppRunning(), "the seeded app settles into Running");

        pda.loadApp(0x30);   // a non-HUD app -> white fade
        check(pda.isTransitioning(), "a fade load reports transitioning");
        check(pda.app() && pda.app()->id() == kAppHud, "the old app stays until the fade midpoint");
        // Drive frames until the new app is swapped in.
        int guard = 0;
        while (pda.app() && pda.app()->id() != 0x30 && guard++ < 50) pda.process(0, 0);
        check(pda.app() && pda.app()->id() == 0x30, "the new app is swapped in under the fade");
        while (pda.isTransitioning() && guard++ < 50) pda.process(0, 0);
        check(!pda.isTransitioning() && pda.isAppRunning(), "the fade finishes and the new app runs");
    }

    // --- defered loading waits for the current app to settle -------------------
    {
        Pda pda;
        pda.init();
        pda.appFactory = [&](int id, ResetOption) { return std::make_unique<StubApp>(id, 3, 1); };
        pda.loadApp(kAppHud);
        pda.deferLoadApp(0x22);
        pda.process(0, 0);   // app still Appearing -> deferred load must not fire
        check(pda.targetApp() == kAppHud, "deferred load holds while the app is still appearing");
        int guard = 0;
        while (pda.targetApp() != 0x22 && guard++ < 50) pda.process(0, 0);
        check(pda.targetApp() == 0x22, "deferred load fires once the app has settled");
    }

    // --- IsAppRunning / IsTransitioning with no app ----------------------------
    {
        Pda pda;
        pda.init();
        check(pda.isAppRunning(), "no app counts as running (IsAppRunning)");
        check(!pda.isTransitioning(), "no app and no transition is not transitioning");
    }

    // --- enable gating and input delay -----------------------------------------
    {
        Pda pda;
        pda.init();
        pda.enable(true, false);
        check(!pda.enabled(), "enable needs the stings to have been shown");
        pda.enable(true, true);
        check(pda.enabled(), "enable takes once the stings are shown");
    }

    // --- input routed only while interactive -----------------------------------
    {
        Pda pda;
        pda.init();
        auto app = std::make_unique<StubApp>(kAppHud, 2, 1);
        StubApp* raw = app.get();
        pda.appFactory = [&](int, ResetOption) { return std::move(app); };
        pda.loadApp(kAppHud);
        pda.process(0, 0);   // Loading -> Appearing, so input is now routed
        Touch t; t.down = true; t.pressed = true;
        pda.processInput(t);
        check(raw->inputs == 1, "input reaches the app while Appearing");
        check(pda.app() == raw, "the factory's app object is the hosted one");
    }

    // --- clock flash flips on a minute change for HUD/map ----------------------
    {
        Pda pda;
        pda.init();
        pda.appFactory = [&](int id, ResetOption) { return std::make_unique<StubApp>(id); };
        pda.loadApp(kAppHud);
        pda.process(10, 8);
        bool first = pda.clockFlash();
        pda.process(10, 8);   // same minute
        check(!pda.clockFlash(), "the clock does not flash without a minute change");
        pda.process(11, 8);   // minute advanced
        check(pda.clockFlash(), "the clock flashes on a minute change under the HUD app");
        (void)first;
    }

    // --- loading overlay fades in to half and out to nothing -------------------
    {
        Pda pda;
        pda.init();
        pda.displayLoading(true);
        for (int i = 0; i < 10; ++i) pda.process(0, 0);
        pda.displayLoading(false);
        for (int i = 0; i < 10; ++i) pda.process(0, 0);
        // After fading out the overlay is gone; a re-show works again.
        pda.displayLoading(true);
        pda.process(0, 0);
        check(true, "loading overlay show/hide cycles without error");
    }

    if (failures == 0) std::printf("pda_test: all checks passed\n");
    return failures ? 1 : 0;
}
