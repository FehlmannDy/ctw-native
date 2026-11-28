// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Exercise the cGangMonitor hate/hostility logic (gangs.cpp) with synthetic data only. Every expected value is
// re-derived here from the libGame.so constants cited in gangs.cpp/.h, independently of the port's own named
// constants where it matters:
//   * the eight gang ped types come from the acceptance bitmask 0x99A9 (cGangMonitor::AddScore 0x8CD868);
//   * player kill = +30 (0x1E), vehicle destroy = +100 (0x64), decay = -5, clamp = [0, 450] (0x1C2);
//   * a gang is hostile to the player exactly while its hate score is > 0.
#include "gangs.h"
#include <cstdint>
#include <cstdio>

namespace {
int failures;
void check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; } }

// Re-derived here from the raw bitmask, not taken from gangs.cpp.
bool expectGang(int pedType) {
    if (pedType < 0 || pedType > 15) return false;
    return ((0x99A9u >> pedType) & 1u) != 0;
}
}  // namespace

int main() {
    using namespace gangs;

    // ---- gang membership: bitmask 0x99A9 -> {0,3,5,7,8,11,12,15} ------------------------------------------------
    int gangCount = 0;
    for (int t = -1; t <= 25; ++t) {
        check(isGangPedType(t) == expectGang(t), "isGangPedType matches 0x99A9 bitmask");
        if (isGangPedType(t)) ++gangCount;
    }
    check(gangCount == 8, "exactly eight gang ped types");
    check(isGangPedType(0) && isGangPedType(3) && isGangPedType(15), "0/3/15 are gangs");
    check(!isGangPedType(13) && !isGangPedType(18) && !isGangPedType(19), "13/18/19 are not AddScore gangs");

    // ---- slot <-> ped type round trip (tables at 0x486F78 / 0x486FF8) ------------------------------------------
    const int expectSlotPedType[kNumGangSlots] = { 0, 3, 5, 7, 8, 11, 12, 15, 19, 18 };
    for (int s = 0; s < kNumGangSlots; ++s)
        check(pedTypeForGangSlot(s) == expectSlotPedType[s], "pedTypeForGangSlot matches reverse table");
    for (int t = 0; t <= 15; ++t)
        if (isGangPedType(t))
            check(pedTypeForGangSlot(gangSlotForPedType(t)) == t, "gang slot round-trips back to ped type");
    check(gangSlotForPedType(7) == 3, "ped type 7 -> slot 3");
    check(gangSlotForPedType(2) == -1 && gangSlotForPedType(18) == -1, "non-gang/high type -> no slot");

    // ---- vehicle id -> gang ped type (table at 0x4AE1A0, base id 0x45) -----------------------------------------
    check(gangPedTypeForVehicleId(0x45) == 0, "vehicle 0x45 -> gang 0");
    check(gangPedTypeForVehicleId(0x45 + 3) == 7, "vehicle id+3 -> gang 7");
    check(gangPedTypeForVehicleId(0x45 + 5) == 15, "vehicle id+5 -> gang 15");
    check(gangPedTypeForVehicleId(0x45 + 1) == kPedTypeNonGang, "vehicle id+1 -> non-gang sentinel");
    check(gangPedTypeForVehicleId(0x44) == kPedTypeNonGang, "below range -> non-gang");
    check(gangPedTypeForVehicleId(0x45 + 20) == kPedTypeNonGang, "above range -> non-gang");

    // ---- AddScore: accumulate, clamp, and the hostility decision ------------------------------------------------
    {
        GangHateMonitor m;
        check(!m.isHostileToPlayer(7), "a fresh gang is at peace");

        ThreatAction act;
        // player kills a gang-7 ped: +30. The SetThreat loop must flag slot 3 (ped type 7) as hostile.
        int v = m.onGangPedKilledByPlayer(7, &act);
        check(v == 30, "one kill adds 30");
        check(m.isHostileToPlayer(7), "gang 7 is now hostile");
        check(act.setThreatMask == (1u << 3), "only slot 3 threat flagged");
        check(act.resetPedType == -1, "no reset while hostile");

        // destroying a gang-7 vehicle: +100 -> 130.
        v = m.onGangVehicleDestroyedByPlayer(0x45 + 3, &act);
        check(v == 130, "vehicle destroy adds 100");

        // ceiling clamp at 450 (0x1C2): +450 would be 580, clamps to 450; one more stays at 450.
        v = m.addScore(7, 450, &act);
        check(v == 450, "clamped to 450 ceiling");
        v = m.addScore(7, 1, &act);
        check(v == 450, "stays at 450 ceiling");

        // floor clamp at 0 and the reset request: a big negative drives it to 0 and asks ResetThreats(7).
        v = m.addScore(7, -1000, &act);
        check(v == 0, "floored at 0");
        check(!m.isHostileToPlayer(7), "gang 7 back at peace");
        check(act.resetPedType == 7, "reset threat requested for ped type 7");
        check((act.setThreatMask & (1u << 3)) == 0, "slot 3 no longer in threat mask");
    }

    // ---- non-gang ped types and vehicles are ignored ------------------------------------------------------------
    {
        GangHateMonitor m;
        check(m.addScore(13, 100, nullptr) == 0, "player ped type 13 is not a trackable gang");
        check(m.addScore(2, 100, nullptr) == 0, "non-gang ped type ignored");
        // vehicle id+1 maps to the non-gang sentinel (20), which AddScore rejects.
        check(m.onGangVehicleDestroyedByPlayer(0x45 + 1, nullptr) == 0, "non-gang vehicle adds nothing");
        for (int s = 0; s < kNumGangSlots; ++s) check(m.scoreForSlot(s) == 0, "no hate recorded");
    }

    // ---- UpdateHate decay: -5 per tick down to peace, and the reset on player death -----------------------------
    {
        GangHateMonitor m;
        m.onGangPedKilledByPlayer(8, nullptr);  // ped type 8 -> slot 4, score 30
        check(m.scoreForPedType(8) == 30, "gang 8 at 30");

        // Six decay ticks of -5 -> 0; the sixth tick crosses to 0 and asks for a reset.
        int hostile = 1;
        for (int i = 0; i < 6; ++i) hostile = m.decayTick(nullptr);
        check(m.scoreForPedType(8) == 0, "decayed to 0 after six ticks");
        check(hostile == 0, "no gang hostile after decay");

        // A partial value that is not a multiple of 5 still clamps to 0 rather than going negative.
        GangHateMonitor m2;
        m2.addScore(11, 3, nullptr);  // slot 5
        ThreatAction a;
        int n = m2.decayTick(&a);
        check(m2.scoreForPedType(11) == 0, "3 - 5 clamps to 0");
        check(n == 0 && a.resetPedType == 11, "decay reports the reset for gang 11");

        // Two gangs active at once: the threat mask carries both slots; reset() clears everything.
        GangHateMonitor m3;
        m3.onGangPedKilledByPlayer(0, nullptr);   // slot 0
        m3.onGangPedKilledByPlayer(12, nullptr);  // slot 6
        ThreatAction a3;
        m3.addScore(0, 0, &a3);
        check(a3.setThreatMask == ((1u << 0) | (1u << 6)), "both active gangs in threat mask");
        m3.reset();
        for (int s = 0; s < kNumGangSlots; ++s) check(m3.scoreForSlot(s) == 0, "reset clears all hate");
    }

    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("gangs_test: all checks passed\n");
    return 0;
}
