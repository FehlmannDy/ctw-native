// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Gui::cDrugVanSlashApp and its inner Gui::cDrugVanSlashApp::cSlash: the PDA minigame of cutting a parked drug van
// open and taking the stash. The van's rear doors are slashed open with four connected swipes that trace a closed
// polyline across the doors; when the last cut lands the doors tear open, the player grabs the stash and the
// inventory is awarded. Matched routine-by-routine against the Android ARM64 libGame.so (addresses cited below).
//
// This file is SELF-CONTAINED: it reproduces only the swipe/state logic with its own 2D vector maths, so it depends
// on no other minigame lane. Rendering, sound, PDA hosting, GXT result text and the inventory award are left as
// reported hooks (see the std::function members and the notes on each routine); nothing is drawn or played here.
//
// The mechanic (observation from the binary unless marked assumption):
//   * Four slashes (cDrugVanSlashApp::Reset 0x7f45b4 builds cSlash objects at +0x1e8/+0x340/+0x498/+0x5f0, stride
//     0x158). Each cSlash::SetSlashLine (0x7f366c) takes two sVector2d endpoints and stores the segment length at
//     +0x44 (= |end-start|) and the direction delta at +0x48. The five waypoints come from a const table at
//     0x485118: (700,369) (700,88) (984,134) (984,325) (700,369) — a closed 4-segment path over the doors. Reset
//     chains them so slash i runs waypoint[i] -> waypoint[i+1].
//   * cSlash::Update (0x7f38b4) drives a one-way stroke variable along the segment and returns true only when the
//     progress (+0x128) has reached the full segment length (+0x44). In the original the drag is a continuous
//     cStrokeRegion fat-line with momentum fed by cRollingPosition::SupplyPosition; the player sweeps the finger
//     along the segment and it is "slashed" once the sweep covers the whole length, one-way (backward motion does
//     not count). (Observation: completion == progress reaching length, one-way. Assumption: the port collapses the
//     continuous drag into a single accepted swipe vector per segment — a faithful abstraction of the same test.)
//   * cDrugVanSlashApp::Process (0x7f4940) state machine on +0x194:
//       0 Slashing      advance the current slash; on completion hide it, reveal the next, and when the slash index
//                       (+0x198) reaches 4 play the cut-open HUD sfx 0x145 (PlayHudSfx, 0x7f4c08) and go to state 1.
//       1 TearingOpen   a two-panel tear animation: +0x19c counts 0->1->2, each panel runs +0x1c4 up to the limit
//                       +0x1c0 = 10 frames (table 0x4665c0) while the door sprites interpolate open. When both
//                       panels finish: if the van kind (+0x754) == 4 -> AwardStash and go straight to state 3, else
//                       enable the grab glow (cSpriteWithGlow_OnRelease::SetGlowOn) and go to state 2.
//       2 Grabbing      cOnRelease::Update: a tap/release on the opened van grabs it -> AwardStash, state 3.
//       3 Result        cMiniGameResultTextHelper::Update; when it finishes, fire the player's mini-game
//                       task-complete callback and set finished (+0x190 = 1).
//   * cDrugVanSlashApp::AwardStash (0x7f4d30) by van kind (+0x754): 4 -> GXT 0x199 result only; 3 -> GXT 0x19b with
//     SetPauseTime(180); otherwise the drug stash: item id (+0x74c) and quantity (+0x74e) are added to the player
//     inventory up to its free storage (ccTradeInventory::GetMaxStorage - GetCurrStorage) with the overflow pushed
//     to the stash inventory (cTradeManager::AddPlayerInventory / AddStashInventory).
//   * cDrugVanSlashApp::IsFinished (0x7f4ef4) returns +0x190. cDrugVanSlashApp::FinishCode (0x7f4efc) returns a
//     constant 1. There is NO timer and NO fail path anywhere in Init/Reset/Process (observation): once the four
//     slashes are made the minigame always completes successfully. (So no timeout test exists to write.)
//
// Hooks the orchestrator must still supply (described, not built here):
//   - the cut-open HUD sound (cAudioManager::PlayHudSfx 0x145) and the per-slash audio channel (+0x748);
//   - the inventory award (cTradeManager player/stash inventories) and the GXT result strings 0x198/0x199/0x19b;
//   - the cMiniGameResultTextHelper result-text lifetime and the mini-game task-complete callback;
//   - all rendering (the van backgrounds SS ids 0xe1/0xe2, the slash sprites, the tear animation) and the PDA shell.
//
// Runtime-only values that are NOT statically resolvable and are therefore port defaults marked as assumptions:
//   - the fat-line region half-width and its "behind/past" margins (cSlash::SetSlashLine reads them from .bss
//     globals at 0xdd0360/0x368/0x370/0x378 which are zero in the file image), and the one-way mode init value
//     (0xdd0388). These control how forgiving the sweep is; the port exposes them as named tolerances below.
#pragma once
#include <cstdint>
#include <functional>

namespace drugvanslash {

// A 2D point in the app's 1024x768 ortho space (y down), matching sVector2d usage in the binary.
struct Vec2 { float x = 0, y = 0; };

// +0x194 main state. Values are the originals.
enum class State : int {
    Slashing    = 0,
    TearingOpen = 1,
    Grabbing    = 2,
    Result      = 3,
};

// +0x754 van-content kind, read by AwardStash. Drugs is the common case (awards +0x74c/+0x74e).
enum class VanKind : int {
    Drugs    = 0,
    Special3 = 3,   // GXT 0x19b, SetPauseTime(180)
    Special4 = 4,   // GXT 0x199, auto-award (skips the grab step)
};

// Four slashes forming the closed door polyline (cDrugVanSlashApp::Reset, table 0x485118). Observation.
inline constexpr int kSlashCount = 4;
// The tear-open animation: two panels, each 10 frames (+0x19c -> 2, +0x1c0 limit = 10). Observation.
inline constexpr int kTearPanels      = 2;
inline constexpr int kTearPanelFrames = 10;
// The HUD sfx played when the fourth slash lands (Process 0x7f4c08, PlayHudSfx id 0x145). Observation.
inline constexpr int kCutOpenSfx = 0x145;

// The five waypoints of the door polyline, from the const table at 0x485118 (observation). Slash i runs
// kWaypoints[i] -> kWaypoints[i+1]. Not an "extracted lookup table" in the asset sense: these are the five
// hand-placed screen coordinates cited by address, reproduced so the port's geometry matches the original 1:1.
inline constexpr Vec2 kWaypoints[kSlashCount + 1] = {
    {700.f, 369.f}, {700.f, 88.f}, {984.f, 134.f}, {984.f, 325.f}, {700.f, 369.f},
};

// Port-side sweep tolerances, mapped onto the real fat-line stroke region cSlash::SetSlashLine builds
// (0x7f366c): cConvexRegion::SetOutline makes a 4-corner quad around the segment, widened perpendicular by
// *0xdd0360 (W0, side A) and *0xdd0368 (W1, side B) and extended longitudinally by *0xdd0370 (W2) before the
// start and past the end; cSlash::Update (0x7f38b4) feeds the touch through cStrokeRegion/cRollingPosition and
// completes the slash when the one-way progress (+0x128) reaches the full segment length (+0x44). Those W0/W1/W2
// floats are runtime .bss pointers (zero in the file image), so their concrete values are NOT statically
// resolvable; the three fields below are the port's documented stand-ins (ASSUMPTION), chosen to reproduce the
// forgiving finger-sized band rather than a razor-thin line. The earlier defaults (0.85 / 60 / 90) were a guess
// that rejected ordinary mouse drags (the user saw the van but could not cut it); these looser values match the
// fat line's perpendicular half-width (maxPerp ~ W0/W1) and its longitudinal slack (maxStartGap ~ W2), and a
// forgiving along-fraction because the one-way stroke only needs to *reach* the end within that W2 margin, not
// trace an exact straight line. The swipe() contract is defined purely in terms of these, so the unit test
// (which derives its thresholds from tolerance.*) stays self-consistent whatever the values are.
struct SwipeTolerance {
    float minAlongFrac = 0.6f;    // the sweep's forward projection must cover >= this fraction of the segment
    float maxPerp      = 85.f;    // max perpendicular distance (px) from the segment line (~ fat-line half-width W0/W1)
    float maxStartGap  = 140.f;   // how far before the segment's start the sweep may begin (px) (~ longitudinal margin W2)
};

// The minigame. Create, start(), feed swipe()s, pump process() for the tear/grab/result, read isFinished().
class DrugVanSlash {
public:
    DrugVanSlash() = default;

    // cDrugVanSlashApp::Reset: arm a fresh attempt. vanKind mirrors +0x754; itemId/qty are the stash contents
    // (+0x74c/+0x74e) passed to the award hook. done(success) fires once the result finishes; success is always
    // true, matching FinishCode() == 1 (there is no fail path).
    void start(VanKind vanKind = VanKind::Drugs, int itemId = 0, int qty = 0,
               std::function<void(bool success)> done = {});
    void stop();
    bool active() const { return active_; }

    State state() const { return state_; }         // +0x194
    int slashesDone() const { return slashIndex_; } // +0x198 (0..kSlashCount)
    int slashesNeeded() const { return kSlashCount; }
    VanKind vanKind() const { return vanKind_; }

    // The segment the next slash must cut (waypoints slashIndex_ -> slashIndex_+1). Valid while Slashing.
    Vec2 segmentStart() const;
    Vec2 segmentEnd() const;
    // The segment's length, == the original's +0x44 (|end-start|).
    float segmentLength() const;

    // Register one sweep against the current slash while in state Slashing. Accepts (and completes the slash) when
    // the sweep runs forward along the current segment far enough and close enough (per SwipeTolerance). A sweep
    // that is too short, reversed, or off the door line is rejected and nothing changes. Returns true if the sweep
    // completed the current slash. When the fourth slash completes, fires playSfx(kCutOpenSfx) and enters
    // TearingOpen. (Mirrors cSlash::Update returning true once progress reaches the segment length, 0x7f38b4, and
    // Process's index-reaches-4 branch, 0x7f4b8c.)
    bool swipe(const Vec2& from, const Vec2& to);
    bool swipe(float x0, float y0, float x1, float y1) { return swipe(Vec2{x0, y0}, Vec2{x1, y1}); }

    // cDrugVanSlashApp::Process for the non-slashing states: one 30 Hz frame. In TearingOpen it advances the
    // two-panel 10-frame animation, then either auto-awards (Special4) and enters Result or enables the grab and
    // enters Grabbing. In Result it completes on the next frame (the real result text lifetime is a hook) and marks
    // finished, firing done(true). A no-op while Slashing (swipes drive that) or when inactive/finished.
    void process();

    // cOnRelease: a tap/release on the opened van in Grabbing -> award and enter Result. No-op in other states.
    void grab();

    bool isFinished() const { return finished_; }  // +0x190
    int finishCode() const { return 1; }           // 0x7f4efc: constant 1

    // How far (0..1) the current tear panel has animated, for the renderer. 0 when not tearing.
    float tearProgress() const;

    SwipeTolerance tolerance;   // tunable; see SwipeTolerance.

    // ---- reported hooks (described, not implemented) --------------------------------------------------------
    // The cut-open HUD sound (PlayHudSfx) and, if wired, the per-slash swipe channel.
    std::function<void(int sample, int volume)> playSfx;
    // cDrugVanSlashApp::AwardStash: hand (vanKind,itemId,qty) to the game's cTradeManager inventory award + GXT.
    std::function<void(VanKind kind, int itemId, int qty)> awardStash;

private:
    void completeSlashes();   // the index-reaches-4 tail of Process state 0
    void beginAward();        // AwardStash + enter Result

    bool active_ = false;
    bool finished_ = false;               // +0x190
    State state_ = State::Slashing;        // +0x194
    int slashIndex_ = 0;                   // +0x198
    int tearPanel_ = 0;                    // +0x19c
    int tearFrame_ = 0;                    // +0x1c4 within the current panel
    VanKind vanKind_ = VanKind::Drugs;     // +0x754
    int itemId_ = 0;                       // +0x74c
    int qty_ = 0;                          // +0x74e
    std::function<void(bool)> done_;
};

}  // namespace drugvanslash
