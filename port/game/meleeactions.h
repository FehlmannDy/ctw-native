// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Player combat-action detection for the mission layer: MeleeActionTracker.
//
// kena02 ("Pursuit Farce", the dojo melee tutorial) asks the engine "did the player just punch / fly-kick /
// roundhouse" and "did the player just shoot the target", and keeps per-move counters. kena01 never queries this,
// so the port had no such hook. This is a self-contained, reachable detector: the orchestrator feeds it the
// player's per-frame combat state (state the port already computes), it latches the punch / fly-kick / roundhouse /
// shot-at-target edges and counts them. A future kena02.cpp Mission subclass reads the counters and compares them
// against the mission's own thresholds. This file reaches into nothing: it is driven entirely by the small
// CombatFrame struct the orchestrator passes in.
//
// HOW THE ORIGINAL DISTINGUISHES THE THREE MELEE MOVES (observations from the Android ARM64 libGame.so):
//   cPlayer::TriggerHitsObjectCallback (0x0088d8e0) fires one of three "hits object" event lists the instant a
//   melee attack connects. It reads the ped's live sprite-animation id: cPed+0x2b8 is the ped's animation object,
//   and +0x38 within it is the current eSpriteAnimId (uint16). It branches on that id (0x88d8fc..0x88d940):
//       anim 0x197 (407) -> roundhouse list (cPlayerListener +0x2e2)
//       anim 0x19d (413) -> fly-kick   list (cPlayerListener +0x2de)
//       any other anim   -> punch      list (cPlayerListener +0x2e6)   (the default / fallthrough)
//   So the discriminator is the ANIMATION ID, not a melee combo enum and not a cMeleeBasedWeapon state. The combo
//   bitfield (cPed+0x1184, stage byte cPed+0x118d) and cPlayer::GetMeleeAnim (0x0088cf44) only SELECT which anim
//   plays: stage 0 -> base+0xe9, stage 1 -> base+0xf3, stage 2 ("special move", vtable+0x340) -> the roundhouse /
//   fly-kick anims. cMeleeBasedWeapon::SpecialMoveSfx (0x008fe4e0) reads the same +0x2b8/+0x38 anim id, confirming
//   the field. The mission counters (cKEN_A02::CountPunches 0x00ac58f4 / CountFlyKicks 0x00ac631c /
//   CountRoundhouse 0x00ac5e0c) merely subscribe to the matching list, then poll the dummy's
//   Entity::GetDamagedInLastFrame() each frame it takes player damage to advance (thresholds 8 / 5 / 6).
//
// WHAT THE PORT MODELS TODAY (observation, port/game/weapons.cpp weaponUpper + player.cpp animate):
//   For the fists (weaponType 0) the port plays only base+233 (0xe9) and base+243 (0xf3) -- exactly combo
//   stages 0 and 1, the two punches ("the combo is not ported", weapons.cpp:630). It never advances to stage 2,
//   so it never produces the special-move anims 0x197 / 0x19d. The two punch anim constants match the
//   disassembly's GetMeleeAnim stages exactly (233=0xe9, 243=0xf3).
//   CONSEQUENCE: of the three melee moves, only the PUNCH is distinguishable from the port's state today.
//   Fly-kick and roundhouse are BLOCKED: the port never generates their anims, so the orchestrator cannot
//   observe one. This tracker still ports the real anim-id discriminator (kAnimRoundhouse / kAnimFlyKick), so it
//   classifies correctly the moment stage-2 melee is ported and meleeAnimId starts carrying those ids. Until then
//   the orchestrator passes meleeAnimId = kAnimUnknown and every fists swing latches as a punch -- honest and
//   faithful to what the port can see.
//
// THE "SHOT A TARGET" TEST (observation): cKEN_A02::PlayerShotDummy (0x00ac8140) reads
//   Entity::GetLastDamageType() on the dummy and tests == 1 (gunshot/bullet); the same type-1 test gates the
//   bullet path in cDummy::Shot (0x00abb594). The port's equivalent is a bullet hit (sDamageInfo type 1): when
//   the player's trace strikes a ped, weapons.cpp traceAndFlash reaches g.peds.bulletHit(..., type 1). The
//   orchestrator supplies that hit as shotHitTarget; shotFired is the reachable-now coarser signal (a bullet
//   weapon discharged this frame).
#pragma once
#include <cstdint>

// The player's per-frame combat state, as the port already computes it. The orchestrator fills this after
// Weapons_PlayerFrame has written Player::weaponType / weaponState / weaponFlags (weapons.cpp tail, ~line 1026).
struct CombatFrame {
    int weaponType = 0;        // cWeapon Type / Player::weaponType: 0 = fists (the only unarmed melee)
    int weaponState = 0;       // cWeapon +0xC / Player::weaponState: 1 = fired this frame
    uint32_t weaponFlags = 0;  // cWeapon +0x10 / Player::weaponFlags: bit n set when state n is entered
    int moveLevel = 0;         // Player::level() 0..3 (stand/walk/run/sprint); carried for future use
    // The player's current melee sprite-anim id, the real discriminator (cPed+0x2b8 -> +0x38). kAnimUnknown when
    // the port cannot supply a special-move id (today: always, so every fists swing is a punch).
    int meleeAnimId = -1;
    bool shotFired = false;     // a bullet weapon discharged this frame (reachable now from weapon state)
    bool shotHitTarget = false; // that bullet struck a ped/target this frame (sDamageInfo type 1)
};

class MeleeActionTracker {
public:
    // The real eSpriteAnimId discriminators (observations, cPlayer::TriggerHitsObjectCallback 0x0088d8e0).
    static constexpr int kAnimUnknown = -1;
    static constexpr int kAnimRoundhouse = 0x197;  // 407
    static constexpr int kAnimFlyKick = 0x19d;     // 413

    // Call once per game frame with the player's combat state. Latches the edges and advances the counters.
    void observe(const CombatFrame& f);
    // cScriptProcess restart: zero the counters and the edge/level history.
    void reset();

    // Edge queries: true only on the frame the action was latched (each observe() recomputes them).
    bool punched() const { return punched_; }
    bool flyKicked() const { return flyKicked_; }
    bool roundhoused() const { return roundhoused_; }
    bool shotTarget() const { return shotTarget_; }
    // True on the frame of any of the three melee moves (the port's single fists-swing connect edge).
    bool meleeHit() const { return punched_ || flyKicked_ || roundhoused_; }

    // Running counters (what kena02 reads: CountPunches / CountFlyKicks / CountRoundhouse / CountShots).
    int countPunches() const { return punches_; }
    int countFlyKicks() const { return flyKicks_; }
    int countRoundhouse() const { return roundhouse_; }
    int countShots() const { return shots_; }

private:
    // The port's fists swing-connect test (weapons.cpp FireSfx condition, ~line 1011).
    static bool swingActive(const CombatFrame& f) {
        return f.weaponType == 0 && (f.weaponState == 1 || (f.weaponFlags & 2));
    }
    bool swingPrev_ = false;   // rising-edge latch: one count per swing, holding does not re-count
    bool shotHitPrev_ = false; // rising-edge latch for a bullet that struck a target
    bool punched_ = false, flyKicked_ = false, roundhoused_ = false, shotTarget_ = false;
    int punches_ = 0, flyKicks_ = 0, roundhouse_ = 0, shots_ = 0;
};
