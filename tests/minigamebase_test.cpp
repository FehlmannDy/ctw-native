// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// The shared minigame framework (minigamebase.h), checked against libGame.so. Every asserted value is
// re-derived here from the binary constants cited in the header:
//   PlayerStatus / IsValid     cGenericPlayerMiniGameTask::IsValid     0x0070d8a4
//   Result mapping             PDAImpl::WasMiniGamePassed/Failed/Quit  0x008b2a34 / 0x008b2aa4 / 0x008b29c4
//   minigame app range         PDAImpl::IsAnyMinigameRunning           0x008b2c0c  (> 0x33)
//   TimerBar                   MiniGameTimerBar::Initialise/Update     0x00805020 / 0x00805048
//   ResultText phases          cMiniGameResultTextHelper::Update       0x0081004c  (+consts 0x4692d0 / 0x47d218 / 0x47e548)
//   OnEndApp / callback        cGenericPlayerMiniGameTask::OnEndApp / CallTaskCompleteCallback 0x0070d908 / 0x0070d600
#include "minigamebase.h"
#include <cmath>
#include <cstdio>

using namespace minigame;

static int failures;
static void check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; } }

int main() {
    // --- enum values (used as raw integers by the binary's field compares) ---
    check((int)PlayerStatus::Any == 0 && (int)PlayerStatus::OnFoot == 1 && (int)PlayerStatus::InVehicle == 2,
          "PlayerStatus ids 0/1/2");
    check((int)Result::Failed == 0 && (int)Result::Passed == 1 && (int)Result::Quit == 2,
          "Result ids 0/1/2 (Was* field +0x5c compares)");

    // --- minigame app range: RunningAppType() > 0x33 is a minigame (IsAnyMinigameRunning) ---
    check(!isMinigameApp(0x33), "0x33 is not a minigame app");
    check(isMinigameApp(0x34), "0x34 is the first minigame app");
    check(isMinigameApp(0x58), "0x58 is a minigame app");
    check(kFirstMinigameApp == 0x34 && kHudApp == 0x10, "app id constants");

    // --- IsValid: status gates on the player's vehicle state ---
    {
        Minigame any(0x34, PlayerStatus::Any, {});
        check(any.isValid(true) && any.isValid(false), "Any valid in and out of a vehicle");
        Minigame onFoot(0x34, PlayerStatus::OnFoot, {});
        check(onFoot.isValid(false) && !onFoot.isValid(true), "OnFoot valid only out of a vehicle");
        Minigame inVeh(0x34, PlayerStatus::InVehicle, {});
        check(inVeh.isValid(true) && !inVeh.isValid(false), "InVehicle valid only in a vehicle");
    }

    // --- OnFinished: finished when the reason code is non-zero ---
    check(!Minigame::onFinished(0) && Minigame::onFinished(1) && Minigame::onFinished(-1),
          "OnFinished == (reason != 0)");

    // --- default result is Passed (ctor sets +0x5c = 1) ---
    check(Minigame(0x34, PlayerStatus::Any, {}).result() == Result::Passed, "default result Passed");

    // --- TimerBar: Initialise(limit) zeroes elapsed; Update returns true once elapsed >= limit ---
    {
        TimerBar t;
        t.initialise(3);
        check(t.limit() == 3 && t.elapsed() == 0 && !t.expired(), "timer init: limit 3, elapsed 0");
        check(!t.update(), "timer frame 1 (1 < 3)");    // elapsed 1
        check(!t.update(), "timer frame 2 (2 < 3)");    // elapsed 2
        check(t.update(), "timer frame 3 expires (3 >= 3)");  // elapsed 3 -> limit <= elapsed
        check(t.elapsed() == 3 && t.expired(), "timer elapsed 3, expired");
        check(t.update(), "timer stays expired past the limit");  // elapsed 4
    }

    // --- ResultText: FadeIn (black 0->0.75 step 0.07) -> Show (text 0->1 step 0.1) -> Pause -> Done ---
    {
        ResultText r;
        int alphaCalls = 0, shows = 0;
        bool lastShow = false;
        float lastAlpha = -1.f;
        r.onAlpha = [&](float a) { ++alphaCalls; lastAlpha = a; };
        r.onShow = [&](bool on) { ++shows; lastShow = on; };
        check(r.phase() == ResultText::Phase::Idle, "starts Idle");
        check(!r.update(), "Idle update not done");   // no phase set yet -> never done
        r.begin();
        check(r.phase() == ResultText::Phase::FadeIn, "begin -> FadeIn");
        r.setPauseTime(2);   // deterministic: 1, 0, then -1 triggers Done

        // FadeIn: black alpha ramps by 0.07 toward 0.75; the text is still invisible so the applied alpha is 0.
        int guard = 0;
        while (r.phase() == ResultText::Phase::FadeIn && guard++ < 1000) {
            bool done = r.update();
            check(!done, "not done during FadeIn");
        }
        check(r.phase() == ResultText::Phase::Show, "FadeIn completes -> Show");
        check(std::fabs(r.blackAlpha() - 0.75f) < 1e-4f, "black alpha clamped to 0.75");
        check(r.visible() && lastShow, "sprites shown entering Show");
        check(shows == 1, "ShowSprite(true) fired once at the FadeIn->Show edge");
        // FadeIn took ceil-ish of 0.75/0.07 = ~11 frames (10 frames reach 0.70, the 11th crosses 0.75).
        check(guard >= 10 && guard <= 12, "FadeIn ~11 frames");

        // Show: text alpha ramps by 0.1 toward 1.0 and is clamped.
        guard = 0;
        while (r.phase() == ResultText::Phase::Show && guard++ < 1000) {
            bool done = r.update();
            check(!done, "not done during Show");
            check(r.textAlpha() <= 1.0f + 1e-4f, "text alpha never exceeds 1.0");
        }
        check(r.phase() == ResultText::Phase::Pause, "Show completes -> Pause");
        check(std::fabs(r.textAlpha() - 1.0f) < 1e-4f, "text alpha clamped to 1.0");

        // Pause: setPauseTime(2) -> decrements 1, 0, then -1 (< 0) triggers Done on the 3rd update.
        check(!r.update(), "pause frame 1 (-> 1, not done)");
        check(!r.update(), "pause frame 2 (-> 0, not done)");
        check(r.update(), "pause frame 3 (-> -1 < 0, Done)");
        check(r.phase() == ResultText::Phase::Done, "reached Done");
        check(r.update() && r.phase() == ResultText::Phase::Done, "Done stays done");
        check(alphaCalls > 0 && std::fabs(lastAlpha - 1.0f) < 1e-4f, "alpha hook pushed, final alpha 1.0");
    }

    // --- CallTaskCompleteCallback forwards the result; gated by the has-callback flag (+0x40) ---
    {
        Result got = Result::Failed;
        int calls = 0;
        Host h;
        h.complete = [&](Result r) { got = r; ++calls; };
        Minigame m(0x34, PlayerStatus::Any, h, /*hasCallback=*/true);
        m.setResult(Result::Quit);
        m.callTaskCompleteCallback();
        check(calls == 1 && got == Result::Quit, "callback fired with the set result");

        calls = 0;
        Minigame noCb(0x34, PlayerStatus::Any, h, /*hasCallback=*/false);
        noCb.callTaskCompleteCallback();
        check(calls == 0, "no callback when the +0x40 flag is clear");
    }

    // --- OnEndApp: return to the HUD app only when the PDA still shows this app and return isn't suppressed ---
    {
        int loaded = -1;
        Host h;
        h.runningAppType = [] { return 0x34; };
        h.loadApp = [&](int app) { loaded = app; };
        Minigame m(0x34, PlayerStatus::Any, h);
        m.onEndApp();
        check(loaded == kHudApp, "OnEndApp loads the HUD app (0x10) when its app is running");

        loaded = -1;
        m.setSuppressHudReturn(true);
        m.onEndApp();
        check(loaded == -1, "suppressed (+0x55) -> no HUD load");

        loaded = -1;
        int other = 0x40;
        Host h2;
        h2.runningAppType = [&] { return other; };
        h2.loadApp = [&](int app) { loaded = app; };
        Minigame m2(0x34, PlayerStatus::Any, h2);
        m2.onEndApp();
        check(loaded == -1, "different running app -> no HUD load");

        // Abort clears the suppress flag (0x0070d964 strb wzr,[+0x55]).
        m.abort();
        check(!m.suppressHudReturn() && m.aborted(), "abort clears suppress and marks aborted");
    }

    // --- PDAImpl Was* queries: live only while this app is the running minigame and a task exists ---
    {
        int running = 0x34;
        Host h;
        h.runningAppType = [&] { return running; };
        Minigame m(0x34, PlayerStatus::Any, h);

        m.setResult(Result::Passed);
        check(m.wasPassed(true) && !m.wasFailed(true) && !m.wasQuit(true), "Passed query");
        m.setResult(Result::Failed);
        check(m.wasFailed(true) && !m.wasPassed(true) && !m.wasQuit(true), "Failed query");
        m.setResult(Result::Quit);
        check(m.wasQuit(true) && !m.wasPassed(true) && !m.wasFailed(true), "Quit query");

        m.setResult(Result::Passed);
        check(!m.wasPassed(false), "no task -> query false");
        running = 0x10;   // HUD, not a minigame app
        check(!m.wasPassed(true), "HUD running -> query false (< 0x34)");
        running = 0x40;   // a minigame app, but not this task's app
        check(!m.wasPassed(true), "a different minigame app running -> query false");
    }

    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("minigamebase: all checks passed\n");
    return 0;
}
