// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// cBuildingSwapManager. See buildings.h for the layout and the libGame.so addresses each routine matches.
#include "buildings.h"

// cBuildingSwapManager::cBuildingSwapManager (0x00899044).
// The original zeroes the first 16 bytes of the group-state array and, for every entry, clears the instance
// pointer (+0x00) and the group/mask halfword (+0x0a). mGroupState/mEntries are value-initialised in the
// header, which is a superset of that (the global is constructed over zeroed memory), so the loop here only
// needs to mirror the explicit clears for the reuse case.
cBuildingSwapManager::cBuildingSwapManager() {
    for (Entry& e : mEntries) {
        e.instance = nullptr;
        e.group = 0;
        e.stateMask = 0;
    }
}

// cBuildingSwapManager::Reset (0x008990d0).
// Identical inner loop to the constructor but, as in the binary, it does NOT touch the group-state array nor
// the wantShow/shown bytes -- only the instance pointer and the group/mask are cleared.
void cBuildingSwapManager::reset() {
    for (Entry& e : mEntries) {
        e.instance = nullptr;
        e.group = 0;
        e.stateMask = 0;
    }
}

// cBuildingSwapManager::RegisterWorldModelInstance (0x008990f0).
bool cBuildingSwapManager::registerInstance(IBuildingSwapInstance* inst) {
    // Pass 1: return early if already present; otherwise remember a free slot. The binary keeps the LAST
    // empty slot seen (csel on every empty entry); which empty slot is used has no behavioural effect.
    Entry* free = nullptr;
    for (Entry& e : mEntries) {
        if (e.instance == inst)
            return e.shown != 0;      // already registered: report its current visibility
        if (e.instance == nullptr)
            free = &e;
    }
    if (!free)
        return false;                 // table full. The binary has no such guard (it would deref null);
                                      // kept defensive, no effect while a free slot exists.

    free->instance = inst;
    free->group = 0;
    free->stateMask = 0;              // empty slots already carry 0 here; matches the binary's state on reuse

    // Pass 2: match the instance's model id against the table to learn its group and state mask. A match sets
    // the group (last matching group wins) and ORs in the state bit; the mask is shared across groups, exactly
    // as the binary accumulates it.
    const uint16_t mid = inst->swapModelId();
    if (table_) {
        for (int g = 0; g < BuildingSwapTable::kGroups; ++g)
            for (int s = 0; s < BuildingSwapTable::kStates; ++s)
                for (int k = 0; k < BuildingSwapTable::kModelsPerState; ++k)
                    if (table_->model[g][s][k] == mid) {
                        free->group = static_cast<uint8_t>(g);
                        free->stateMask |= static_cast<uint8_t>(1u << s);
                    }
    }

    // Apply the initial visibility for the group's current state.
    const int cur = mGroupState[free->group];
    const bool visible = ((free->stateMask >> cur) & 1u) != 0;
    if (visible) {
        free->wantShow = 1;           // strh 0x0101 -> both bytes set; the instance is assumed already shown
        free->shown = 1;
        return true;
    }
    inst->hide();
    free->wantShow = 0;
    free->shown = 0;
    return false;
}

// cBuildingSwapManager::UnRegisterWorldModelInstance (0x0089938c).
// A linear search (fully unrolled in the binary) for the instance; the found slot's instance pointer and its
// four metadata bytes (wantShow/shown/group/stateMask, the 32-bit store at entry+0x08) are cleared. No
// show/hide is performed.
void cBuildingSwapManager::unregisterInstance(IBuildingSwapInstance* inst) {
    for (Entry& e : mEntries) {
        if (e.instance == inst) {
            e.instance = nullptr;
            e.wantShow = 0;
            e.shown = 0;
            e.group = 0;
            e.stateMask = 0;
            return;
        }
    }
}

// cBuildingSwapManager::SwitchGroupState (0x008995d8).
void cBuildingSwapManager::switchGroupState(int group, int state) {
    const uint32_t bit = 1u << state;
    for (Entry& e : mEntries) {
        if (e.instance == nullptr) continue;
        if (e.group != group) continue;
        if ((bit & e.stateMask) == 0) continue;
        if (e.shown != 0) continue;       // already shown entries are skipped
        e.wantShow = 1;                   // entry+0x50 = 1
    }
    mGroupState[group] = state;           // entry[group] state, manager+0x08 + group*4
}

// cBuildingSwapManager::Process (0x00899300).
void cBuildingSwapManager::process() {
    for (Entry& e : mEntries) {
        if (e.instance == nullptr) continue;
        const int cur = mGroupState[e.group];
        const bool want = ((e.stateMask >> cur) & 1u) != 0;
        if (want) {
            if (e.shown != 0) continue;
            e.shown = 1;
            e.instance->show();
        } else {
            if (e.shown == 0) continue;
            e.shown = 0;
            e.instance->hide();
        }
    }
}
