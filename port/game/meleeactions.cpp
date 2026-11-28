// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
// Player combat-action detection for the mission layer (see meleeactions.h for the disassembly provenance).
// A port of the discrimination done by cPlayer::TriggerHitsObjectCallback (0x0088d8e0): classify the connecting
// melee attack by the player's live sprite-anim id, and count the shot-at-target events the dummy tutorial polls.
#include "meleeactions.h"

void MeleeActionTracker::reset() {
    swingPrev_ = false;
    shotHitPrev_ = false;
    punched_ = flyKicked_ = roundhoused_ = shotTarget_ = false;
    punches_ = flyKicks_ = roundhouse_ = shots_ = 0;
}

void MeleeActionTracker::observe(const CombatFrame& f) {
    // Edge flags hold for the frame of the event only.
    punched_ = flyKicked_ = roundhoused_ = shotTarget_ = false;

    // Melee: the port exposes a single fists swing-connect edge. Latch it on its rising edge so one swing counts
    // once and holding the animation out does not re-count (matches "a punch edge increments once").
    const bool swing = swingActive(f);
    if (swing && !swingPrev_) {
        // cPlayer::TriggerHitsObjectCallback reads the ped's current eSpriteAnimId (cPed+0x2b8 -> +0x38) and
        // branches: 0x197 roundhouse, 0x19d fly-kick, else punch. The port supplies that id as meleeAnimId; today
        // it only ever generates the two punch anims, so meleeAnimId is kAnimUnknown and this falls through to the
        // punch branch, which is correct for what the port can see.
        if (f.meleeAnimId == kAnimRoundhouse) { roundhoused_ = true; ++roundhouse_; }
        else if (f.meleeAnimId == kAnimFlyKick) { flyKicked_ = true; ++flyKicks_; }
        else { punched_ = true; ++punches_; }
    }
    swingPrev_ = swing;

    // Shot at target: cKEN_A02::PlayerShotDummy (0x00ac8140) counts a frame the dummy's GetLastDamageType() == 1
    // (bullet). The port's equivalent is a bullet hit on a ped (sDamageInfo type 1). Rising-edge latched so one
    // bullet that lands counts once even if the hit signal is held across the fire-delay frames.
    if (f.shotHitTarget && !shotHitPrev_) { shotTarget_ = true; ++shots_; }
    shotHitPrev_ = f.shotHitTarget;
}
