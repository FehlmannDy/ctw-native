// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// cCheckpointBlips / cCheckpointTT: the race (time-trial) checkpoint markers and their pickup coronas.
//
// This file ports the self-contained geometry and state of a checkpoint's life: the per-frame proximity test
// that spawns/culls the pickup prop (cCheckpointBlips::BlipMe), the arming of the circular trigger, the circular
// enter test, and the collect/advance bookkeeping that finishes a race (cCheckpointBlips::Collected). The engine
// pieces it leans on in the original -- the blip (HUDImpl::AddBlip/ChangeBlipStyle), the pickup DynamicProp
// (WorldImpl::CreateDynamicProp model 0x3e), the trigger area (Area::SetToCircularArea + Entity::WhenEnters),
// the collect SFX (SoundImpl::PlaySimpleSFX id 0x26b) and the script-state hand-off (cScriptProcessBase::SetState)
// -- live in off-limits engine/HUD files and are reported as hooks, not called here.
//
// Positions are tv3d<cFixed<20,12>>: 0x1000 == 1.0 world unit. Constants below are read from libGame.so
// cCheckpointBlips::BlipMe (0x00c54010) and cCheckpointTT::SetupTT_Part2 (0x00c53440).
#pragma once
#include <cstdint>
#include <vector>

namespace checkpoints {

// cFixed<20,12>
constexpr int32_t kFix = 0x1000;            // 1.0

// The circular trigger a checkpoint arms at its centre (BlipMe -> Area::SetToCircularArea, 0x00c5422c, point 0xa000).
constexpr int32_t kEnterRadius = 0xa000;    // 10.0 -- player entity entering this circle collects the checkpoint
// The prop/area cull box half-extent: the pickup prop exists only while the player is inside it (BlipMe 0x00c54124).
constexpr int32_t kObjectBox = 0x46000;     // 70.0
// The inner band that arms the circular trigger once the player is close enough (BlipMe 0x00c541f4).
constexpr int32_t kArmBand = 0x19000;       // 25.0
// The whole-race circular boundary that switches off car generators (SetupTT_Part2 0x00c534cc, SetCarGensOff).
constexpr int32_t kRaceAreaRadius = 0x3c000; // 60.0

// AddObject's pickup prop model and ChangeBlipStyle's range (observations, for the reported render/world hooks).
constexpr int kPickupPropModel = 0x3e;      // ePropModel 62
constexpr int32_t kBlipStyleRange = 0x1000; // 1.0 (BlipMe -> HUDImpl::ChangeBlipStyle, 0x00c540c0)
constexpr int kCollectSfxId = 0x26b;        // SoundImpl::PlaySimpleSFX id 619 (Collected, 0x00c553c0)

struct Vec3 { int32_t x = 0, y = 0, z = 0; };

class CheckpointRace;   // the owning time-trial (cCheckpointTT); back-pointer at cCheckpointBlips +0x130

// What a BlipMe tick decides this frame. The booleans mirror the engine side-effects the original performs so a
// caller can drive the reported hooks; the flags are latched into the Checkpoint's own state regardless.
struct Tick {
    bool spawnProp = false;    // cCheckpointBlips::AddObject: create the pickup prop (entered the 70.0 box)
    bool despawnProp = false;  // DynamicProp::FadeOutAndDestroy: left the 70.0 box
    bool armArea = false;      // Area::SetToCircularArea + SetState: trigger armed this frame (entered the 25.0 band)
    bool disarmArea = false;   // Location::Delete: trigger torn down (left the box while armed)
};

// cCheckpointBlips: one checkpoint marker. Flags mirror the original fields: areaActive_ is +0x128, collected_ is
// +0x129. (+0x12a, the Entity::WhenEnters registration, is an engine detail folded into areaActive_ here.)
class Checkpoint {
public:
    void init(const Vec3& centre, CheckpointRace* owner);

    // cCheckpointBlips::BlipMe (0x00c54010): the per-frame proximity test. Returns the engine actions to perform.
    // A collected checkpoint does nothing.
    Tick blipMe(const Vec3& player);

    // The circular enter test the armed trigger performs (horizontal distance <= kEnterRadius). In the original the
    // engine's Area/Entity::WhenEnters raises this; the geometry (radius 10.0, X/Y only) is ported here.
    bool entered(const Vec3& player) const;

    // cCheckpointBlips::Collected (0x00c553a4): mark collected, tear the prop down, and advance the race.
    // Returns true when this collect finished the race (the reported SetState(final) hook should fire).
    bool collect();

    bool isCollected() const { return collected_; }
    bool areaActive() const { return areaActive_; }   // +0x128: the circular trigger is armed
    bool propActive() const { return propActive_; }   // the pickup prop currently exists
    const Vec3& centre() const { return centre_; }

private:
    Vec3 centre_{};
    bool propActive_ = false;
    bool areaActive_ = false;   // +0x128
    bool collected_ = false;    // +0x129
    CheckpointRace* owner_ = nullptr;   // +0x130
};

// cCheckpointTT: the race that owns a run of checkpoints (array stride 0x140, count at +0x118). It drives them each
// frame (UpdateRace) and keeps the collect tally (+0x3330), the target (+0x118) and the finished lock-out (+0x3334).
class CheckpointRace {
public:
    void setup(const Vec3* centres, int count);

    int count() const { return (int)cps_.size(); }
    int total() const { return total_; }
    Checkpoint& at(int i) { return cps_[(size_t)i]; }
    const Checkpoint& at(int i) const { return cps_[(size_t)i]; }

    // cCheckpointTT::UpdateRace (0x00c548c4): tick every checkpoint against the player, then (modelling the engine's
    // WhenEnters) collect any armed checkpoint the player has entered. Returns true when the race is now complete.
    bool updateRace(const Vec3& player);

    // cCheckpointBlips::Collected's tail (0x00c55408): ++count, finished when count >= total and not locked out.
    bool advance();

    int collectedCount() const { return collectedCount_; }   // +0x3330
    bool complete() const { return collectedCount_ >= total_ && !lockout_; }
    bool lockout() const { return lockout_; }                // +0x3334
    void setLockout(bool v) { lockout_ = v; }

private:
    std::vector<Checkpoint> cps_;
    int total_ = 0;            // +0x118
    int collectedCount_ = 0;   // +0x3330
    bool lockout_ = false;     // +0x3334
};

}   // namespace checkpoints
