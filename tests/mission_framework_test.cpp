// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
// The reusable mission-framework logic added to the Mission base (mission.h/.cpp), on synthetic inputs. No original
// data is embedded: every expected number is re-derived from the ported routines.
//   - the countdown clock run from Mission::tick (TimerImpl::Wait style: a timer of N frames is due on the Nth tick);
//   - the objective counter a multi-objective mission keeps;
//   - pass / fail outcomes and the cash reward of cActStructure::DoRewardForMission (0xcb8948): a replay earns
//     nothing, otherwise the per-mission cash is the reward, and the mission passes either way.
#include "mission.h"
#include <cstdio>

// The base Mission::tick only forwards this to the (overridden, no-op) onFrame; it is never dereferenced here, so a
// stand-in definition is enough to call tick. mission.cpp sees Game only as an incomplete reference type.
class Game {};

static int failures;
static void check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; }
}

// Re-exposes the protected framework API so the test can drive it the way a mission script would.
class TestMission : public Mission {
public:
    void start(Game&) override {}
    using Mission::startTimer;
    using Mission::stopTimer;
    using Mission::setObjectiveCount;
    using Mission::completeObjective;
    using Mission::passWithReward;
    using Mission::pass;
    using Mission::fail;
    void onFrame(Game&) override {}
};

static void timerTests() {
    Game g;
    // a 5-frame clock: counted down one frame a tick, due on the 5th (matches wait()/TimerImpl::Wait)
    {
        TestMission m;
        int fired = 0;
        m.startTimer(5, [&] { ++fired; });
        check(m.timerActive() && m.timerRemaining() == 5, "a fresh 5-frame clock shows 5 frames left");
        for (int i = 1; i <= 4; ++i) {
            m.tick(g);
            check(fired == 0, "the clock has not expired before its last frame");
            check(m.timerRemaining() == 5 - i, "the clock loses one frame a tick");
        }
        m.tick(g);   // the 5th tick
        check(fired == 1, "the clock expires on its last frame");
        check(!m.timerActive() && m.timerRemaining() == -1, "an expired clock is no longer running");
        m.tick(g);
        check(fired == 1, "an expired clock does not fire again");
    }
    // stopping a clock drops it with no callback
    {
        TestMission m;
        int fired = 0;
        m.startTimer(3, [&] { ++fired; });
        m.tick(g);
        m.stopTimer();
        check(!m.timerActive(), "a stopped clock is not running");
        m.tick(g); m.tick(g); m.tick(g);
        check(fired == 0, "a stopped clock never fires");
    }
    // a 1-frame clock is due on the very next tick; a 0-frame clock on the first tick too
    {
        TestMission m; int fired = 0;
        m.startTimer(1, [&] { ++fired; });
        m.tick(g);
        check(fired == 1, "a 1-frame clock fires on the first tick");
    }
    {
        TestMission m; int fired = 0;
        m.startTimer(0, [&] { ++fired; });
        m.tick(g);
        check(fired == 1, "a 0-frame clock fires on the first tick");
    }
    // a time limit that fails the mission on expiry: no further ticks run and the outcome is a fail
    {
        TestMission m;
        m.startTimer(2, [&] { m.fail(); });
        m.tick(g);
        check(m.outcome() == Mission::kRunning, "the mission is still running before the clock expires");
        m.tick(g);
        check(m.outcome() == Mission::kFailed, "the clock's expiry can fail the mission");
        check(m.finished(), "a failed mission is finished");
        check(!m.timerActive(), "finishing drops the clock");
    }
}

static void objectiveTests() {
    TestMission m;
    check(!m.allObjectivesDone(), "a mission with no objectives set is not complete");
    m.setObjectiveCount(3);
    check(m.objectivesTotal() == 3 && m.objectivesDone() == 0, "three goals set, none done");
    m.completeObjective();
    m.completeObjective();
    check(m.objectivesDone() == 2 && !m.allObjectivesDone(), "two of three done is not yet complete");
    m.completeObjective();
    check(m.objectivesDone() == 3 && m.allObjectivesDone(), "the third goal completes the mission's objectives");
    m.completeObjective();
    check(m.objectivesDone() == 3, "completing past the total does not overcount");
    m.setObjectiveCount(1);
    check(m.objectivesDone() == 0 && !m.allObjectivesDone(), "setting the count again resets the progress");
}

static void rewardTests() {
    // cActStructure::DoRewardForMission: a normal pass grants the mission's cash and passes
    {
        TestMission m;
        const int32_t granted = m.passWithReward(1500);
        check(granted == 1500, "a normal pass grants the mission's cash");
        check(m.rewardCash() == 1500, "the granted cash is recorded");
        check(m.outcome() == Mission::kPassed && m.finished(), "a rewarded pass is a finished, passed mission");
    }
    // IsOnReplayMission: a replay earns nothing, but still passes
    {
        TestMission m;
        const int32_t granted = m.passWithReward(1500, /*replay=*/true);
        check(granted == 0, "a replay earns no cash");
        check(m.rewardCash() == 0, "no cash is recorded on a replay");
        check(m.outcome() == Mission::kPassed, "a replay still passes the mission");
    }
    // a plain pass (cActStructure::PassMission, no reward to show, as kena01 does)
    {
        TestMission m;
        m.pass();
        check(m.outcome() == Mission::kPassed && m.rewardCash() == 0, "a plain pass shows no reward");
        check(m.finished(), "a passed mission is finished");
    }
    // a fail
    {
        TestMission m;
        m.fail();
        check(m.outcome() == Mission::kFailed && m.finished(), "a failed mission is finished and marked failed");
    }
    // the win/lose HUD constants the end path uses, re-stated from the disassembly
    check(Mission::kPassStringId == 0x5D, "the MISSION PASSED string id is 0x5D");
    check(Mission::kFailStringId == 0x5C, "the MISSION FAILED string id is 0x5C");
    check(Mission::kRewardStringId == 0x5E, "the reward line string id is 0x5E");
    check(Mission::kRewardDisplayFrames == 50, "DoRewardForMission shows the reward for 50 frames");
    check(Mission::kPassedDisplayFrames == 90, "a plain pass is shown for 90 frames");
    check(Mission::kFailedDisplayFrames == 70, "a fail is shown for 70 frames");
    check(Mission::kWinLoseFont == 5, "the win/lose text is drawn in font 5");
}

int main() {
    timerTests();
    objectiveTests();
    rewardTests();
    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("mission framework: all checks passed\n");
    return 0;
}
