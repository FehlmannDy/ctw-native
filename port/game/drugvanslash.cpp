// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Gui::cDrugVanSlashApp / cSlash: the self-contained swipe + state logic of the drug-van slashing minigame.
// See drugvanslash.h for the per-routine evidence (addresses) and the observation-vs-assumption notes.
#include "drugvanslash.h"

#include <cmath>
#include <utility>

namespace drugvanslash {
namespace {

// Small self-contained 2D helpers (the port does not pull in the engine's sVector2d).
inline float dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
inline Vec2 sub(const Vec2& a, const Vec2& b) { return Vec2{a.x - b.x, a.y - b.y}; }
inline float length(const Vec2& v) { return std::sqrt(v.x * v.x + v.y * v.y); }

}  // namespace

// cDrugVanSlashApp::Reset (0x7f45b4): zero the state, index and tear counters and arm the four slashes.
void DrugVanSlash::start(VanKind vanKind, int itemId, int qty, std::function<void(bool)> done) {
    active_ = true;
    finished_ = false;
    state_ = State::Slashing;   // +0x194 = 0
    slashIndex_ = 0;            // +0x198 = 0
    tearPanel_ = 0;             // +0x19c = 0
    tearFrame_ = 0;             // +0x1c4 = 0
    vanKind_ = vanKind;
    itemId_ = itemId;
    qty_ = qty;
    done_ = std::move(done);
}

void DrugVanSlash::stop() {
    active_ = false;
    done_ = {};
}

Vec2 DrugVanSlash::segmentStart() const {
    int i = slashIndex_;
    if (i < 0) i = 0;
    if (i > kSlashCount - 1) i = kSlashCount - 1;
    return kWaypoints[i];
}

Vec2 DrugVanSlash::segmentEnd() const {
    int i = slashIndex_;
    if (i < 0) i = 0;
    if (i > kSlashCount - 1) i = kSlashCount - 1;
    return kWaypoints[i + 1];
}

float DrugVanSlash::segmentLength() const {
    return length(sub(segmentEnd(), segmentStart()));   // the original's +0x44
}

// cSlash::Update (0x7f38b4) returns true when the one-way progress reaches the segment length. The port accepts one
// sweep per segment: project it onto the segment direction (one-way: the forward component must be positive and
// cover minAlongFrac of the length), require the sweep's start to begin no earlier than maxStartGap before the
// segment start, and require it to stay within maxPerp of the segment line.
bool DrugVanSlash::swipe(const Vec2& from, const Vec2& to) {
    if (!active_ || state_ != State::Slashing) return false;

    const Vec2 a = segmentStart();
    const Vec2 b = segmentEnd();
    const Vec2 seg = sub(b, a);
    const float segLen = length(seg);
    if (segLen <= 0.f) return false;

    const Vec2 dir = Vec2{seg.x / segLen, seg.y / segLen};          // unit along the segment
    const Vec2 sweep = sub(to, from);

    // Forward projection of the sweep along the segment (one-way: must be positive). Rejects a reversed sweep.
    const float along = dot(sweep, dir);
    if (along < tolerance.minAlongFrac * segLen) return false;      // too short, or wrong direction (along <= 0)

    // Where the sweep starts, measured along the segment from a. Must not begin too far before the segment.
    const float startAlong = dot(sub(from, a), dir);
    if (startAlong < -tolerance.maxStartGap) return false;

    // Perpendicular distance of the sweep's endpoints from the infinite segment line. Keeps the cut on the door.
    auto perp = [&](const Vec2& p) {
        const Vec2 rel = sub(p, a);
        const float proj = dot(rel, dir);
        const Vec2 foot = Vec2{a.x + dir.x * proj, a.y + dir.y * proj};
        return length(sub(p, foot));
    };
    if (perp(from) > tolerance.maxPerp || perp(to) > tolerance.maxPerp) return false;

    // Accepted: this slash is cut. Advance the index (Process 0x7f4b8c).
    ++slashIndex_;
    if (slashIndex_ >= kSlashCount) completeSlashes();
    return true;
}

// Process state 0 tail (0x7f4ba0..0x7f4c44): the fourth slash landed. Play the cut-open sfx and enter TearingOpen.
void DrugVanSlash::completeSlashes() {
    slashIndex_ = kSlashCount;
    if (playSfx) playSfx(kCutOpenSfx, 0x7f);   // PlayHudSfx(0x145, 0x7f)
    state_ = State::TearingOpen;               // +0x194 = 1
    tearPanel_ = 0;
    tearFrame_ = 0;
}

// cDrugVanSlashApp::AwardStash (0x7f4d30) + enter Result (+0x194 = 3).
void DrugVanSlash::beginAward() {
    if (awardStash) awardStash(vanKind_, itemId_, qty_);
    state_ = State::Result;
}

// cDrugVanSlashApp::Process for states 1..3 (0x7f4a5c onward).
void DrugVanSlash::process() {
    if (!active_ || finished_) return;

    switch (state_) {
        case State::Slashing:
            // Driven by swipe(); nothing to advance per frame.
            break;

        case State::TearingOpen: {
            // Two panels, each counting +0x1c4 up to the limit +0x1c0 (= kTearPanelFrames).
            if (++tearFrame_ >= kTearPanelFrames) {
                tearFrame_ = 0;
                if (++tearPanel_ >= kTearPanels) {
                    // Both doors open. Special4 auto-awards; everything else goes to the grab (0x7f4ab8 branch).
                    if (vanKind_ == VanKind::Special4) {
                        beginAward();
                    } else {
                        state_ = State::Grabbing;   // SetGlowOn, +0x194 = 2
                    }
                }
            }
            break;
        }

        case State::Grabbing:
            // Waits for grab() (cOnRelease). Nothing automatic.
            break;

        case State::Result:
            // The real result-text lifetime (cMiniGameResultTextHelper::Update) is a hook; here it resolves on the
            // next pumped frame, then marks finished and fires the task-complete callback (0x7f4b08).
            finished_ = true;      // +0x190 = 1
            if (done_) done_(true);
            break;
    }
}

// cOnRelease::Update true branch (0x7f4c60): grab the opened van.
void DrugVanSlash::grab() {
    if (!active_ || finished_ || state_ != State::Grabbing) return;
    beginAward();
}

float DrugVanSlash::tearProgress() const {
    if (state_ != State::TearingOpen) return 0.f;
    return static_cast<float>(tearFrame_) / static_cast<float>(kTearPanelFrames);
}

}  // namespace drugvanslash
