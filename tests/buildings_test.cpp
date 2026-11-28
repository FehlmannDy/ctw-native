// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// cBuildingSwapManager, synthetic data only. Exercises the ported decision logic (register/match, group
// switching and process show/hide) with a fake IBuildingSwapInstance; no world or GL code is touched.
#include "buildings.h"
#include <cstdio>

struct BuildingSwapTestAccess {
    static int usedSlots(const cBuildingSwapManager& m) {
        int n = 0;
        for (const auto& e : m.mEntries) n += (e.instance != nullptr);
        return n;
    }
    static uint8_t mask(const cBuildingSwapManager& m, const IBuildingSwapInstance* inst) {
        for (const auto& e : m.mEntries) if (e.instance == inst) return e.stateMask;
        return 0;
    }
    static uint8_t group(const cBuildingSwapManager& m, const IBuildingSwapInstance* inst) {
        for (const auto& e : m.mEntries) if (e.instance == inst) return e.group;
        return 0xff;
    }
    static uint8_t wantShow(const cBuildingSwapManager& m, const IBuildingSwapInstance* inst) {
        for (const auto& e : m.mEntries) if (e.instance == inst) return e.wantShow;
        return 0;
    }
};

namespace {
struct FakeInstance : IBuildingSwapInstance {
    uint16_t id;
    bool visible = true;   // world-model instances start shown; the manager hides what should not be seen
    int shows = 0, hides = 0;
    explicit FakeInstance(uint16_t i) : id(i) {}
    uint16_t swapModelId() const override { return id; }
    void show() override { visible = true; ++shows; }
    void hide() override { visible = false; ++hides; }
};

int failures = 0;
void check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; } }
}

int main() {
    using TA = BuildingSwapTestAccess;

    // Synthetic table. Group 0: model 100 appears in state 0, model 101 in state 1, model 102 in states 0+1.
    // Group 1: model 200 in state 2 only. Everything else is an unused (zero) slot.
    BuildingSwapTable table{};
    table.model[0][0][0] = 100;   // group 0, state 0
    table.model[0][1][0] = 101;   // group 0, state 1
    table.model[0][0][3] = 102;   // group 0, state 0 (a non-first slot, to exercise the inner scan)
    table.model[0][1][3] = 102;   // group 0, state 1  -> model 102 mask = (1<<0)|(1<<1) = 0b0011 = 3
    table.model[1][2][0] = 200;   // group 1, state 2  -> mask = (1<<2) = 4

    cBuildingSwapManager mgr;
    mgr.init(&table);

    FakeInstance a(100), b(101), c(102), d(200), unknown(999);

    // --- register: matching, mask derivation, and initial visibility at state 0 (default) ---
    // Group 0 current state is 0. a(mask 1) is visible in state 0 -> stays shown, returns true.
    // b(mask 2) is NOT visible in state 0 -> hidden immediately, returns false.
    // c(mask 3) is visible in state 0 -> shown. d is group 1 state 2; group 1 state is 0 -> hidden.
    bool ra = mgr.registerInstance(&a);
    bool rb = mgr.registerInstance(&b);
    bool rc = mgr.registerInstance(&c);
    bool rd = mgr.registerInstance(&d);
    bool ru = mgr.registerInstance(&unknown);   // no table match: mask 0, group 0 -> never visible

    check(ra == true,  "a visible at group0/state0");
    check(rb == false, "b hidden at group0/state0");
    check(rc == true,  "c visible at group0/state0");
    check(rd == false, "d (group1) hidden while group1 state is 0");
    check(ru == false, "unknown model never visible (mask 0)");

    check(TA::mask(mgr, &a) == 0b0001, "a mask = state0 bit");
    check(TA::mask(mgr, &b) == 0b0010, "b mask = state1 bit");
    check(TA::mask(mgr, &c) == 0b0011, "c mask = state0|state1");
    check(TA::mask(mgr, &d) == 0b0100, "d mask = state2 bit");
    check(TA::mask(mgr, &unknown) == 0, "unknown mask empty");
    check(TA::group(mgr, &d) == 1, "d assigned to group 1");
    check(TA::usedSlots(mgr) == 5, "five slots used");

    // Register side effects: only non-visible instances were hidden at register time.
    check(a.hides == 0 && a.visible, "a not hidden at register");
    check(b.hides == 1 && !b.visible, "b hidden once at register");
    check(c.hides == 0 && c.visible, "c not hidden at register");
    check(d.hides == 1 && !d.visible, "d hidden once at register");

    // re-registering an already-present instance returns its shown flag and does not add a slot
    check(mgr.registerInstance(&a) == true, "re-register a returns shown=true");
    check(mgr.registerInstance(&b) == false, "re-register b returns shown=false");
    check(TA::usedSlots(mgr) == 5, "re-register adds no slot");

    // --- switchGroupState + process: move group 0 to state 1 ---
    mgr.switchGroupState(0, 1);
    check(mgr.groupState(0) == 1, "group 0 now state 1");
    // switchGroupState marks matching, not-yet-shown entries. b (mask has state1, currently hidden) gets
    // wantShow=1; a is currently shown so it is skipped even though its bit is clear for state 1.
    check(TA::wantShow(mgr, &b) == 1, "b marked wantShow by switch");

    mgr.process();
    // After process at group0/state1: a(mask 1) hidden, b(mask 2) shown, c(mask 3) still shown, d unchanged.
    check(!a.visible && a.hides == 1, "a hidden after switch to state 1");
    check(b.visible && b.shows == 1, "b shown after switch to state 1");
    check(c.visible && c.shows == 0, "c stays shown across state 0->1 (mask 3)");
    check(!d.visible, "d (group 1) still hidden");

    // idempotent: a second process with no state change performs no show/hide
    int bShows = b.shows, aHides = a.hides;
    mgr.process();
    check(b.shows == bShows && a.hides == aHides, "process is idempotent without a state change");

    // --- group 1 to state 2 shows d ---
    mgr.switchGroupState(1, 2);
    mgr.process();
    check(d.visible && d.shows == 1, "d shown after group 1 -> state 2");

    // --- unregister frees the slot and does not toggle visibility ---
    int dShowsBefore = d.shows, dHidesBefore = d.hides;
    mgr.unregisterInstance(&d);
    check(TA::usedSlots(mgr) == 4, "unregister frees d's slot (4 of 5 remain)");
    check(d.shows == dShowsBefore && d.hides == dHidesBefore, "unregister does not show/hide");
    mgr.process();   // d no longer tracked: process must not touch it
    check(d.shows == dShowsBefore && d.hides == dHidesBefore, "process ignores unregistered d");

    // --- reset clears entries (but not group states) ---
    mgr.reset();
    check(TA::usedSlots(mgr) == 0, "reset clears all entries");
    check(mgr.groupState(0) == 1 && mgr.groupState(1) == 2, "reset leaves group states (matches binary)");

    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("buildings: all checks passed\n");
    return 0;
}
