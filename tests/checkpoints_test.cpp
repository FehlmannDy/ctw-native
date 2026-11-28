// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Race checkpoint geometry and state, checked against libGame.so:
//   cCheckpointBlips::BlipMe     0x00c54010  (cull box 0x46000=70.0, arm band 0x19000=25.0, trigger 0xa000=10.0)
//   cCheckpointBlips::Collected  0x00c553a4  (++count at +0x3330, finish when count>=total +0x118 && !lock +0x3334)
//   cCheckpointTT::UpdateRace    0x00c548c4  (BlipMe each checkpoint, stride 0x140)
// Synthetic positions only; the blip/prop/SFX/area engine calls are hooks and are not exercised.
// Every expectation is re-derived from the named checkpoints:: constants so nothing can drift out of sync.
#include "checkpoints.h"
#include <cstdio>

using namespace checkpoints;

static int failures;
static void check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; } }

int main() {
    // Sanity on the raw constants (cFixed<20,12>, 0x1000 == 1.0).
    check(kFix == 4096, "1.0 unit");
    check(kEnterRadius == 10 * kFix, "enter radius 10.0");
    check(kObjectBox == 70 * kFix, "cull box 70.0");
    check(kArmBand == 25 * kFix, "arm band 25.0");
    check(kRaceAreaRadius == 60 * kFix, "race area 60.0");

    const Vec3 origin{0, 0, 0};

    // --- enter test: circular, X/Y only, radius 10.0 ---
    {
        Checkpoint cp;
        cp.init(origin, nullptr);
        check(cp.entered(Vec3{9 * kFix, 0, 0}), "9.0 along x is inside the 10.0 circle");
        check(cp.entered(Vec3{0, -10 * kFix, 0}), "exactly 10.0 is inside (<=)");
        check(!cp.entered(Vec3{11 * kFix, 0, 0}), "11.0 is outside");
        // 6-8-10 triangle: (6,8) is exactly on the circle; (7,8) is just outside.
        check(cp.entered(Vec3{6 * kFix, 8 * kFix, 0}), "6,8 on the circle");
        check(!cp.entered(Vec3{7 * kFix, 8 * kFix, 0}), "7,8 outside");
        // Z is ignored by the circular area.
        check(cp.entered(Vec3{0, 0, 9999 * kFix}), "z ignored");
    }

    // --- BlipMe lifetime: prop spawns in the 70.0 box, trigger arms in the 25.0 band, both tear down on exit ---
    {
        Checkpoint cp;
        cp.init(origin, nullptr);

        // Far away: nothing.
        Tick t = cp.blipMe(Vec3{100 * kFix, 0, 0});
        check(!t.spawnProp && !cp.propActive() && !cp.areaActive(), "far: inert");

        // Enter the cull box (60.0 < 70.0) but outside the arm band: prop spawns, trigger stays disarmed.
        t = cp.blipMe(Vec3{60 * kFix, 0, 0});
        check(t.spawnProp && cp.propActive(), "60.0: prop spawns");
        check(!t.armArea && !cp.areaActive(), "60.0: trigger not armed yet");

        // Staying in the box does not re-spawn the prop.
        t = cp.blipMe(Vec3{55 * kFix, 0, 0});
        check(!t.spawnProp && cp.propActive(), "55.0: prop persists, no respawn");

        // Reach the arm band (20.0 < 25.0): the circular trigger arms, once.
        t = cp.blipMe(Vec3{20 * kFix, 0, 0});
        check(t.armArea && cp.areaActive(), "20.0: trigger arms");
        t = cp.blipMe(Vec3{15 * kFix, 0, 0});
        check(!t.armArea && cp.areaActive(), "15.0: already armed, no re-arm");

        // Leave the box entirely (80.0 > 70.0): prop and trigger both drop.
        t = cp.blipMe(Vec3{80 * kFix, 0, 0});
        check(t.despawnProp && t.disarmArea, "80.0: prop and trigger torn down");
        check(!cp.propActive() && !cp.areaActive(), "80.0: inactive again");
    }

    // --- box boundary: 70.0 edge is inside (strict <), just past it is out ---
    {
        Checkpoint cp;
        cp.init(origin, nullptr);
        Tick t = cp.blipMe(Vec3{(70 * kFix) - 1, 0, 0});
        check(t.spawnProp, "just under 70.0: in box");
        cp.init(origin, nullptr);
        t = cp.blipMe(Vec3{70 * kFix, 0, 0});
        check(!t.spawnProp && !cp.propActive(), "exactly 70.0: out of box");
    }

    // --- advance + completion across a 3-checkpoint race (UpdateRace drives it) ---
    {
        const Vec3 line[3] = {Vec3{0, 0, 0}, Vec3{200 * kFix, 0, 0}, Vec3{400 * kFix, 0, 0}};
        CheckpointRace race;
        race.setup(line, 3);
        check(race.total() == 3 && race.count() == 3, "3 checkpoints, total 3");
        check(race.collectedCount() == 0 && !race.complete(), "nothing collected yet");

        // Drive to each checkpoint in turn. One pass to arm (inside 25.0 band but with entity not yet entered is
        // not modelled separately here: arriving at the centre both arms and enters), then collect on arrival.
        // Checkpoint 0.
        bool fin = race.updateRace(Vec3{0, 0, 0});
        check(race.at(0).isCollected() && race.collectedCount() == 1, "checkpoint 0 collected");
        check(!fin && !race.complete(), "race not finished after 1/3");
        // Re-entering a collected checkpoint does nothing.
        fin = race.updateRace(Vec3{0, 0, 0});
        check(race.collectedCount() == 1, "collected checkpoint is inert");

        // Checkpoint 1.
        fin = race.updateRace(Vec3{200 * kFix, 0, 0});
        check(race.at(1).isCollected() && race.collectedCount() == 2, "checkpoint 1 collected");
        check(!fin, "race not finished after 2/3");

        // Checkpoint 2 finishes the race.
        fin = race.updateRace(Vec3{400 * kFix, 0, 0});
        check(race.at(2).isCollected() && race.collectedCount() == 3, "checkpoint 2 collected");
        check(fin && race.complete(), "race finished after 3/3");
    }

    // --- an armed checkpoint only collects once the entity is inside the 10.0 trigger, not merely in the band ---
    {
        const Vec3 one[1] = {Vec3{0, 0, 0}};
        CheckpointRace race;
        race.setup(one, 1);
        // 20.0 away: inside the arm band so the trigger arms, but 20.0 > 10.0 so no collect yet.
        bool fin = race.updateRace(Vec3{20 * kFix, 0, 0});
        check(race.at(0).areaActive() && !race.at(0).isCollected(), "armed but not entered at 20.0");
        check(!fin && race.collectedCount() == 0, "no collect from the band alone");
        // Step into the 10.0 circle: now it collects and finishes.
        fin = race.updateRace(Vec3{5 * kFix, 0, 0});
        check(race.at(0).isCollected() && fin, "collected on entering the circle");
    }

    // --- lock-out (+0x3334) withholds completion even when every checkpoint is collected ---
    {
        const Vec3 two[2] = {Vec3{0, 0, 0}, Vec3{300 * kFix, 0, 0}};
        CheckpointRace race;
        race.setup(two, 2);
        race.setLockout(true);
        race.updateRace(Vec3{0, 0, 0});
        bool fin = race.updateRace(Vec3{300 * kFix, 0, 0});
        check(race.collectedCount() == 2, "both collected under lock-out");
        check(!fin && !race.complete(), "lock-out withholds completion");
        race.setLockout(false);
        check(race.complete(), "clearing the lock lets completion register");
    }

    if (failures) { std::fprintf(stderr, "%d checkpoint check(s) failed\n", failures); return 1; }
    std::printf("all checkpoint checks passed\n");
    return 0;
}
