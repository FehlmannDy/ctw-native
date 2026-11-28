// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// cGangMonitor hate/hostility logic. See gangs.h for the overview and the libGame.so addresses behind every
// constant and table. The three threat side effects remain cross-file hooks (see the note at the bottom of this
// file): this translation unit computes the decision and records the requested action, it never touches the
// engine's ped-info threat matrix.
#include "gangs.h"

namespace gangs {
namespace {

// gang ePedType -> hate slot. Table at 0x486F78 in libGame.so (read by cGangMonitor::AddScore at 0x8CD874). Entries
// for non-gang ped types are never read -- the 0x99A9 bitmask guards the lookup -- so they are left as -1 here.
constexpr int kSlotForPedType[16] = {
    /* 0*/ 0, /* 1*/ -1, /* 2*/ -1, /* 3*/ 1, /* 4*/ -1, /* 5*/ 2, /* 6*/ -1, /* 7*/ 3,
    /* 8*/ 4, /* 9*/ -1, /*10*/ -1, /*11*/ 5, /*12*/ 6, /*13*/ -1, /*14*/ -1, /*15*/ 7,
};

// hate slot -> gang ePedType. Reverse table at 0x486FF8 (and the decay list at 0x4AE208 holds the same ten values).
constexpr int kPedTypeForSlot[kNumGangSlots] = { 0, 3, 5, 7, 8, 11, 12, 15, 19, 18 };

// destroyed-vehicle id (minus 0x45) -> gang ePedType. Table at 0x4AE1A0 (read by cGangMonitor::HandleDataEvent at
// 0xCE705C). kPedTypeNonGang (20) marks a vehicle that belongs to no tracked gang.
constexpr int kGangVehiclePedType[kGangVehicleCount] = {
    0, 20, 20, 7, 7, 15, 20, 12, 11, 3, 8, 20, 12, 11, 20, 11, 8, 3, 20, 15,
};

// Bit s of the returned mask is set when slot s currently has a positive hate score (the SetThreat loop at 0x8CD8EC
// re-asserts a threat for every such gang on each AddScore).
uint16_t activeMask(const int32_t* scores) {
    uint16_t m = 0;
    for (int s = 0; s < kNumGangSlots; ++s)
        if (scores[s] > 0) m |= uint16_t(1u << s);
    return m;
}

}  // namespace

bool isGangPedType(int pedType) {
    // cGangMonitor::AddScore 0x8CD860/0x8CD868: reject ped type > 15, then test bit `pedType` of 0x99A9.
    if (pedType < 0 || pedType > 15) return false;
    return ((0x99A9u >> pedType) & 1u) != 0;
}

int gangSlotForPedType(int pedType) {
    return isGangPedType(pedType) ? kSlotForPedType[pedType] : -1;
}

int pedTypeForGangSlot(int slot) {
    return (slot >= 0 && slot < kNumGangSlots) ? kPedTypeForSlot[slot] : -1;
}

int gangPedTypeForVehicleId(int vehicleId) {
    // cGangMonitor::HandleDataEvent 0xCE704C: index = GetId()-0x45, out-of-range -> kPedTypeNonGang (0xCE7068).
    const int idx = vehicleId - kGangVehicleFirstId;
    if (idx < 0 || idx >= kGangVehicleCount) return kPedTypeNonGang;
    return kGangVehiclePedType[idx];
}

int GangHateMonitor::addScore(int pedType, int delta, ThreatAction* out) {
    ThreatAction act;
    const int slot = gangSlotForPedType(pedType);
    int result = 0;
    if (slot >= 0) {
        // 0x8CD884-0x8CD8A4: score += delta; <=0 clamps to 0 (and asks for a threat reset); >=451 clamps to 450.
        int v = scores_[slot] + delta;
        if (v <= 0) {
            v = 0;
            act.resetPedType = pedType;  // cPedInfoManager::ResetThreats(pedType) (0x8CD8C8)
        } else if (v >= kScoreMax + 1) {
            v = kScoreMax;
        }
        scores_[slot] = v;
        result = v;
    }
    // 0x8CD8D0: the SetThreat loop runs regardless of whether the score update was accepted.
    act.setThreatMask = activeMask(scores_);
    if (out) *out = act;
    return result;
}

int GangHateMonitor::decayTick(ThreatAction* out) {
    // cGangMonitor::UpdateHate 0xCF52E0: for each slot with a nonzero score, AddScore(pedType, -5).
    int lastReset = -1;
    for (int s = 0; s < kNumGangSlots; ++s) {
        if (scores_[s] != 0) {
            ThreatAction a;
            addScore(kPedTypeForSlot[s], kScoreDecayPerTick, &a);
            if (a.resetPedType >= 0) lastReset = a.resetPedType;
        }
    }
    const uint16_t mask = activeMask(scores_);
    if (out) { out->resetPedType = lastReset; out->setThreatMask = mask; }
    int n = 0;
    for (int s = 0; s < kNumGangSlots; ++s) n += (scores_[s] > 0);
    return n;
}

void GangHateMonitor::reset() {
    // cGangMonitor::UpdateHate 0xCF5204-0xCF52A0: zero every hate score (the engine also resets each gang's threat).
    for (int s = 0; s < kNumGangSlots; ++s) scores_[s] = 0;
}

}  // namespace gangs

// ---------------------------------------------------------------------------------------------------------------
// CROSS-FILE HOOKS (reported, not wired -- these live in peds.cpp / the engine and are off-limits here):
//
//   * cGangMonitor::HandleDataEvent (0xCE6ED0) is driven by a cDeadEntityDataEvent: it must confirm the killer
//     Entity equals the player (Entity::operator==, 0xCE6FD8) and that the dead Entity is a Ped (GetType in
//     0x34..0x37) or a Vehicle, then feed GangHateMonitor::onGangPedKilledByPlayer / onGangVehicleDestroyedByPlayer.
//     Wiring it needs the Ped/Vehicle/Entity accessors (Ped::GetPedType 0xD47990, Vehicle::GetId 0xD492F0) owned by
//     peds.cpp. -- HOOK, not made.
//   * cGangMonitor::AddScore asks for cPedInfoManager::SetThreat(gangPedType, 13) / ResetThreats(gangPedType)
//     (0xD43DF0 / 0xD43E40) and UpdateHate for WorldImpl::ResetThreatForPedType (0xD4A460). Those apply the
//     gang<->ped threat matrix in the engine; GangHateMonitor returns the requested ThreatAction instead. -- HOOK.
//   * UpdateHate is a cScriptProcess coroutine: it Stops, waits kResetIntervalFrames / kDecayIntervalFrames via
//     TimerImpl::Wait, and re-enters. The frame loop belongs to the script scheduler. -- HOOK.
// ---------------------------------------------------------------------------------------------------------------
