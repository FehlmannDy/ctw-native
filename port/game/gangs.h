// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// cGangMonitor: the global gang hate/hostility tracker (a cScriptProcess, ctor is the cScriptProcess<cGangMonitor>
// template). It is NOT a territory system and holds no per-zone data (observation: the class carries only a flat
// array of per-gang hate scores and a script coroutine); gang-vs-gang friend/enemy relationships are not stored
// here either -- they live in the pedinfo threat matrix reached through the cPedInfoManager / WorldImpl threat
// hooks below. What cGangMonitor owns is the decision "is gang G hostile to the player right now", driven by how
// much the player has provoked that gang, decaying back to peace over time.
//
// Hate layout (observed): cGangMonitor stores 10 int32 hate scores at this+0xE8..+0x10C (cGangMonitor::UpdateHate
// 0xCF51CC writes exactly those ten offsets; cGangMonitor::AddScore 0x8CD850 indexes this+0xE8 + slot*4). Only 8 of
// the 10 slots are ever written -- the slot-index table at 0x486F78 maps a gang ePedType to a slot, and it has
// entries only for the eight gang ped types {0,3,5,7,8,11,12,15}. Slots 8 and 9 (ped types 19 and 18 per the
// reverse table at 0x486FF8) are unreachable through AddScore because its acceptance bitmask 0x99A9 rejects any
// ePedType above 15; they stay zero (observation).
//
// All constants and tables below are re-derived from libGame.so (Android 4.4.243) at the cited addresses. No engine
// headers are pulled in: this is pure decision logic so it can be unit-tested on its own. The three threat side
// effects (SetThreat / ResetThreats / ResetThreatForPedType) are cross-file hooks into peds.cpp's
// cPedInfoManager / WorldImpl and are NOT performed here -- they are reported, not wired.
#pragma once
#include <cstdint>

namespace gangs {

// ---- constants (observed) ---------------------------------------------------------------------------------------
enum { kNumGangSlots = 10 };            // score array length (this+0xE8..+0x10C), cGangMonitor::UpdateHate 0xCF51CC
constexpr int kPedTypeNonGang = 20;     // 0x14: "not a tracked gang" sentinel in the vehicle map; AddScore rejects it
constexpr int kThreatTargetPedType = 13;// AddScore passes 13 as the second SetThreat arg (assumption: the player ped type)

constexpr int kScoreKillGangPed        = 30;  // +0x1E: HandleDataEvent, player kills a gang ped        (0xCE7000)
constexpr int kScoreDestroyGangVehicle = 100; // +0x64: HandleDataEvent, player destroys a gang vehicle (0xCE7070)
constexpr int kScoreDecayPerTick       = -5;  //  -5 : UpdateHate decay step per active gang            (0xCF52F8)
constexpr int kScoreMax                = 450; // 0x1C2: clamp ceiling; AddScore rejects a sum >= 451     (0x8CD898)
constexpr int kDecayIntervalFrames     = 30;  // 0x1E: UpdateHate wait between decay ticks               (0xCF5348)
constexpr int kResetIntervalFrames     = 60;  // 0x3C: UpdateHate wait after the player-death reset      (0xCF52C4)

// Vehicle ids the HandleDataEvent vehicle path recognises (table at 0x4AE1A0 is indexed by GetId()-0x45).
constexpr int kGangVehicleFirstId = 0x45;
enum { kGangVehicleCount = 20 };

// ---- table lookups (observed) ---------------------------------------------------------------------------------
// true for the eight gang ped types AddScore accepts: bitmask 0x99A9 tested at 0x8CD868 (ped types 0,3,5,7,8,11,12,15).
bool isGangPedType(int pedType);
// gang ePedType -> hate slot 0..9, or -1 if the ped type is not a tracked gang. Table at 0x486F78 (0x8CD874).
int gangSlotForPedType(int pedType);
// hate slot 0..9 -> gang ePedType, or -1 if out of range. Reverse table at 0x486FF8 (0x8CD8E4): slots 8,9 -> 19,18.
int pedTypeForGangSlot(int slot);
// destroyed-vehicle id -> gang ePedType, or kPedTypeNonGang if the id is unknown. Table at 0x4AE1A0 (0xCE705C).
int gangPedTypeForVehicleId(int vehicleId);

// ---- cGangMonitor -----------------------------------------------------------------------------------------------
// A threat side effect requested by AddScore / UpdateHate. The engine satisfies these via the hooks named in
// gangs.cpp; this struct only records what the ported decision logic decided should happen, so callers (and tests)
// can see the outcome without the engine present.
struct ThreatAction {
    // AddScore drove a gang's score to 0: cPedInfoManager::ResetThreats(pedType) should run (0x8CD8C8).
    int resetPedType = -1;
    // After the update at least one gang has score > 0: cPedInfoManager::SetThreat(gangPedType, kThreatTargetPedType)
    // should run for every such gang. activeMask bit s is set when slot s is hostile (0x8CD8EC loop).
    uint16_t setThreatMask = 0;
};

class GangHateMonitor {
public:
    GangHateMonitor() { reset(); }

    // cGangMonitor::AddScore (0x8CD850): add `delta` to gang `pedType`'s hate, clamp to [0, 450]. A non-gang ped
    // type is ignored (bitmask reject). Returns the gang's new score (0 if the ped type is not a gang). `out`, when
    // given, receives the threat side effects this call implies.
    int addScore(int pedType, int delta, ThreatAction* out = nullptr);

    // cGangMonitor::HandleDataEvent (0xCE6ED0) decisions -- caller has already confirmed the killer is the player.
    int onGangPedKilledByPlayer(int pedType, ThreatAction* out = nullptr) {
        return addScore(pedType, kScoreKillGangPed, out);
    }
    int onGangVehicleDestroyedByPlayer(int vehicleId, ThreatAction* out = nullptr) {
        return addScore(gangPedTypeForVehicleId(vehicleId), kScoreDestroyGangVehicle, out);
    }

    // cGangMonitor::UpdateHate (0xCF51CC), per-frame branch: subtract kScoreDecayPerTick from every active gang.
    // Returns the number of gangs still hostile afterwards. `out` records the resulting threat actions.
    int decayTick(ThreatAction* out = nullptr);

    // cGangMonitor::UpdateHate (0xCF51CC), player death/arrest branch: clear every gang's hate.
    void reset();

    int scoreForSlot(int slot) const { return (slot >= 0 && slot < kNumGangSlots) ? scores_[slot] : 0; }
    int scoreForPedType(int pedType) const { return scoreForSlot(gangSlotForPedType(pedType)); }

    // The hostility decision: a gang attacks/threatens the player exactly while its hate score is positive.
    bool isHostileToPlayer(int pedType) const { return scoreForPedType(pedType) > 0; }
    bool slotHostile(int slot) const { return scoreForSlot(slot) > 0; }

    int32_t scores_[kNumGangSlots];  // this+0xE8..+0x10C
};

}  // namespace gangs
