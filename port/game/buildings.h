// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// cBuildingSwapManager, checked routine-by-routine against libGame.so (Android 4.4.243):
//   cBuildingSwapManager::cBuildingSwapManager          0x00899044  (36 B)
//   cBuildingSwapManager::Init                          0x00899068  (104 B)  -- resource load is a hook
//   cBuildingSwapManager::Reset                         0x008990d0  (32 B)
//   cBuildingSwapManager::RegisterWorldModelInstance    0x008990f0  (528 B)
//   cBuildingSwapManager::UnRegisterWorldModelInstance  0x0089938c  (588 B)
//   cBuildingSwapManager::Process                       0x00899300  (140 B)
//   cBuildingSwapManager::SwitchGroupState              0x008995d8  (100 B)
//
// What the manager does (observation, from the disassembly above): it toggles the visibility of a fixed
// set of world-model instances ("buildings") so that, for each swap group, only the instances that belong
// to the group's current state are shown. A loaded resource table maps model ids to (group, state) pairs;
// an instance registers itself, is matched against that table to learn its group and the bitmask of states
// it appears in, and is then shown/hidden as the group's state changes (e.g. an intact vs. a destroyed
// variant of the same building footprint).
//
// Separability: the decision logic is fully ported here. The actual show/hide of a world-model instance
// (cWorldModelInstance::Show/Hide, 0xd44bf0/0xd44bd0) and the model-id read (cWorldModelInstance+0x18) live
// in off-limits world code, so they are reached through IBuildingSwapInstance. Likewise the resource load
// in Init (cResourceManager::Load) is a hook; init() here takes the already-resolved table.
#pragma once
#include <cstdint>

// The world-model instance seen by the manager. In the game this is implemented by cWorldModelInstance
// (world code, not edited here): swapModelId() returns the halfword at cWorldModelInstance+0x18, and
// show()/hide() forward to cWorldModelInstance::Show/Hide(nullptr).
class IBuildingSwapInstance {
public:
    virtual ~IBuildingSwapInstance() = default;
    virtual uint16_t swapModelId() const = 0;   // [inst + 0x18], the model id matched against the table
    virtual void show() = 0;                     // cWorldModelInstance::Show(nullptr)
    virtual void hide() = 0;                      // cWorldModelInstance::Hide(nullptr)
};

// The loaded swap table (the resource stored at manager+0x00). Layout derived from the Register scan:
// 16 groups * 4 states * 8 model-id slots of uint16, stride group=0x40, state=0x10, slot=2 bytes.
// model[g][s][k] == 0 is treated as an unused slot by convention; a registering instance whose model id
// equals model[g][s][k] belongs to group g and has state bit (1<<s) set in its mask.
struct BuildingSwapTable {
    static constexpr int kGroups = 16;
    static constexpr int kStates = 4;
    static constexpr int kModelsPerState = 8;
    uint16_t model[kGroups][kStates][kModelsPerState];
};

class cBuildingSwapManager {
public:
    static constexpr int kGroupCount = 16;   // group-state array occupies manager+0x08 .. +0x47 (16 * int32)
    static constexpr int kEntryCount = 24;   // entry table occupies manager+0x48 .. +0x1C7 (24 * 16 bytes)

    cBuildingSwapManager();                  // cBuildingSwapManager::cBuildingSwapManager

    // cBuildingSwapManager::Init -- PARTIAL. The original resolves a resource id from a global config
    // record and calls cResourceManager::Load; that load is a hook. This stores the resolved table.
    void init(const BuildingSwapTable* table) { table_ = table; }

    void reset();                            // cBuildingSwapManager::Reset

    // cBuildingSwapManager::RegisterWorldModelInstance. Returns whether the instance is (or should be) shown
    // in its group's current state. If already visible it is left shown; otherwise it is hidden immediately.
    bool registerInstance(IBuildingSwapInstance* inst);

    // cBuildingSwapManager::UnRegisterWorldModelInstance. Frees the slot (does not show/hide).
    void unregisterInstance(IBuildingSwapInstance* inst);

    // cBuildingSwapManager::SwitchGroupState. Records the new state for the group and marks the matching,
    // not-yet-shown entries (see mWantShow). Visibility itself is applied by process().
    void switchGroupState(int group, int state);

    // cBuildingSwapManager::Process. Shows/hides each registered instance to match its group's current state.
    void process();

    int groupState(int group) const { return mGroupState[group]; }

private:
    // One registered instance. Byte layout mirrors manager+0x48 + 16*i:
    //   +0x00 instance (8)  +0x08 wantShow  +0x09 shown  +0x0a group  +0x0b stateMask
    struct Entry {
        IBuildingSwapInstance* instance = nullptr;  // +0x00
        uint8_t wantShow = 0;                        // +0x08: set by switchGroupState/register; not read by
                                                     //        these routines (assumption: read by rendering)
        uint8_t shown = 0;                           // +0x09: the applied visibility, driven by process()
        uint8_t group = 0;                           // +0x0a
        uint8_t stateMask = 0;                       // +0x0b: bitmask over states (1<<state)
    };

    const BuildingSwapTable* table_ = nullptr;       // +0x00
    int32_t mGroupState[kGroupCount] = {};           // +0x08: current state per group
    Entry mEntries[kEntryCount] = {};                // +0x48

    friend struct BuildingSwapTestAccess;
};
