// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
#include "checkpoints.h"
#include <cstdint>

namespace checkpoints {

namespace {
// BlipMe's box tests use the ARM64 "add then unsigned-compare" idiom to bound a signed delta in one branch.
// |d| strictly inside the 70.0 cull box: (uint32_t)(d + 0x45fff) <= 0x8bffe  (BlipMe 0x00c54124).
inline bool inCullX(int32_t d) { return (uint32_t)(d + 0x45fff) <= 0x8bffeu; }
// The cull-box Y test is the plain pair dy in (-70.0, 70.0) (BlipMe 0x00c5414c..0x00c54160).
inline bool inCullY(int32_t d) { return d > -kObjectBox && d < kObjectBox; }
// The arm band X test: (uint32_t)(d + 0x19000 - 1) < 0x31fff, i.e. d in (-25.0, 25.0) (BlipMe 0x00c541f4).
inline bool inArmX(int32_t d) { return (uint32_t)(d + kArmBand - 1) < 0x31fffu; }
// The arm band Y test: dy in (-25.0, 25.0) (BlipMe 0x00c5420c..0x00c5421c).
inline bool inArmY(int32_t d) { return d > -kArmBand && d < kArmBand; }
}   // namespace

void Checkpoint::init(const Vec3& centre, CheckpointRace* owner) {
    centre_ = centre;
    owner_ = owner;
    propActive_ = false;
    areaActive_ = false;
    collected_ = false;
}

// cCheckpointBlips::BlipMe (0x00c54010).
Tick Checkpoint::blipMe(const Vec3& player) {
    Tick t;
    if (collected_) return t;   // 0x00c5403c: a collected checkpoint is inert

    const int32_t dx = player.x - centre_.x;
    const int32_t dy = player.y - centre_.y;

    if (!(inCullX(dx) && inCullY(dy))) {
        // Left the 70.0 box (0x00c541a0): drop the armed trigger and the prop.
        if (areaActive_) { t.disarmArea = true; areaActive_ = false; }
        if (propActive_) { t.despawnProp = true; propActive_ = false; }
        return t;
    }

    // Inside the box: the pickup prop exists (cCheckpointBlips::AddObject, 0x00c54170).
    if (!propActive_) { t.spawnProp = true; propActive_ = true; }

    // Arm the circular trigger once, the first time the player reaches the 25.0 band (0x00c541f4).
    if (!areaActive_ && inArmX(dx) && inArmY(dy)) {
        t.armArea = true;        // Area::SetToCircularArea(centre_, radius kEnterRadius); then SetState(arming)
        areaActive_ = true;      // +0x128
    }
    return t;
}

// The circular trigger the armed area raises: horizontal (X/Y) distance within kEnterRadius.
bool Checkpoint::entered(const Vec3& player) const {
    const int64_t dx = (int64_t)player.x - centre_.x;
    const int64_t dy = (int64_t)player.y - centre_.y;
    const int64_t r = kEnterRadius;
    return dx * dx + dy * dy <= r * r;
}

// cCheckpointBlips::Collected (0x00c553a4).
bool Checkpoint::collect() {
    if (collected_) return false;
    collected_ = true;    // +0x129; (hook) PlaySimpleSFX kCollectSfxId, FadeOutAndDestroy the prop, RemoveBlip
    propActive_ = false;
    areaActive_ = false;
    return owner_ ? owner_->advance() : false;
}

void CheckpointRace::setup(const Vec3* centres, int count) {
    cps_.clear();
    cps_.resize(count < 0 ? 0 : (size_t)count);
    total_ = (int)cps_.size();   // +0x118
    collectedCount_ = 0;
    lockout_ = false;
    for (size_t i = 0; i < cps_.size(); ++i)
        cps_[i].init(centres[i], this);
}

// cCheckpointBlips::Collected tail (0x00c55408): advance the tally and report whether the race is finished.
bool CheckpointRace::advance() {
    ++collectedCount_;           // +0x3330
    return complete();           // count >= total (+0x118) and not locked out (+0x3334)
}

// cCheckpointTT::UpdateRace (0x00c548c4): BlipMe every checkpoint, then collect any armed one the player entered.
bool CheckpointRace::updateRace(const Vec3& player) {
    for (auto& cp : cps_) cp.blipMe(player);
    bool finished = false;
    for (auto& cp : cps_) {
        if (!cp.isCollected() && cp.areaActive() && cp.entered(player)) {
            if (cp.collect()) finished = true;   // true only when this was the finishing checkpoint
        }
    }
    return finished;
}

}   // namespace checkpoints
