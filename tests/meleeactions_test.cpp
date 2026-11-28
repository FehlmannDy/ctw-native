// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// MeleeActionTracker, the player combat-action detector kena02 ("Pursuit Farce") needs.
// Constants re-derived here from the disassembly (cPlayer::TriggerHitsObjectCallback 0x0088d8e0):
//   roundhouse eSpriteAnimId = 0x197 (407), fly-kick = 0x19d (413), punch = any other anim (fallthrough).
// Port fists swing-connect edge = weaponType 0 && (weaponState 1 || weaponFlags bit1) (weapons.cpp FireSfx test).
// Shot-at-target = a bullet hit (sDamageInfo type 1), matching cKEN_A02::PlayerShotDummy GetLastDamageType()==1.
// Synthetic state sequences only; no engine objects.
#include "meleeactions.h"
#include <cstdio>

static int failures;
static void check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; } }

// A frame of fists state: state 1 is the fire/connect frame; flags bit1 (2) mirrors it in the history window.
static CombatFrame fist(int state, uint32_t flags = 0, int anim = MeleeActionTracker::kAnimUnknown) {
    CombatFrame f; f.weaponType = 0; f.weaponState = state; f.weaponFlags = flags; f.meleeAnimId = anim; return f;
}
static CombatFrame idle() { CombatFrame f; f.weaponType = 0; f.weaponState = 0; return f; }

int main() {
    // --- a punch edge increments punches exactly once ---
    {
        MeleeActionTracker t;
        t.observe(fist(0));            // ready, no swing
        check(t.countPunches() == 0, "no punch before a swing");
        t.observe(fist(1));            // swing connects: rising edge
        check(t.punched(), "punch edge latched on the connect frame");
        check(t.countPunches() == 1, "one swing -> one punch");
        check(!t.flyKicked() && !t.roundhoused(), "an unknown-anim swing is a punch, not a kick");
    }

    // --- holding the swing out does not re-count ---
    {
        MeleeActionTracker t;
        t.observe(fist(1));            // connect
        t.observe(fist(1, 2));         // still in the swing (state held / flags bit1)
        t.observe(fist(2, 2));         // fire-delay, history still shows state 1
        check(t.countPunches() == 1, "a held swing counts once");
        check(!t.punched(), "no new edge while the same swing plays out");
        // a fresh swing after returning to ready counts again
        t.observe(idle());             // ready, history cleared
        t.observe(fist(1));            // second connect
        check(t.countPunches() == 2, "a second distinct swing counts again");
    }

    // --- fly-kick vs roundhouse distinction (keyed off the real anim ids) ---
    {
        MeleeActionTracker t;
        t.observe(fist(1, 0, MeleeActionTracker::kAnimFlyKick));   // 0x19d
        check(t.flyKicked() && t.countFlyKicks() == 1, "anim 0x19d -> fly-kick");
        check(t.countPunches() == 0 && t.countRoundhouse() == 0, "a fly-kick is not a punch or roundhouse");
        t.observe(idle());
        t.observe(fist(1, 0, MeleeActionTracker::kAnimRoundhouse)); // 0x197
        check(t.roundhoused() && t.countRoundhouse() == 1, "anim 0x197 -> roundhouse");
        check(t.countFlyKicks() == 1 && t.countPunches() == 0, "roundhouse leaves the other counters alone");
    }

    // --- shot-at-target counts once per landed bullet ---
    {
        MeleeActionTracker t;
        CombatFrame miss; miss.weaponType = 5; miss.shotFired = true;            // fired, hit nothing
        t.observe(miss);
        check(t.countShots() == 0, "a shot that hits nothing does not count");
        CombatFrame hit; hit.weaponType = 5; hit.shotFired = true; hit.shotHitTarget = true;
        t.observe(hit);
        check(t.shotTarget() && t.countShots() == 1, "a bullet that lands counts one shot");
        t.observe(hit);                                                           // signal held
        check(t.countShots() == 1, "a held hit signal does not re-count");
        t.observe(miss);                                                          // hit released
        CombatFrame hit2 = hit;
        t.observe(hit2);                                                          // second distinct hit
        check(t.countShots() == 2, "a second landed bullet counts again");
    }

    // --- reset zeroes the counters and the edge history ---
    {
        MeleeActionTracker t;
        t.observe(fist(1));
        t.reset();
        check(t.countPunches() == 0 && t.countShots() == 0, "reset zeroes counters");
        t.observe(fist(1));   // a swing that was mid-edge before reset must latch cleanly afterwards
        check(t.countPunches() == 1, "reset clears the rising-edge history");
    }

    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("melee actions OK\n");
    return 0;
}
