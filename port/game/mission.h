// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
// The mission script runtime: a mission is a chain of callbacks (cScriptProcessBase::Call / SetState) run from
// TimerImpl::Wait, from the WhenEnters / WhenEntersVehicle style conditions and from its sequences' callbacks.
// The game side provides what the script asks of CameraImpl (FadeToBlack, FadeIn), HUDImpl (DisplayObjective,
// AddBlip) and InitMissionText.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Game;

// cScriptProcessBase for a mission's helper scripts (cInWaterHelpText, ...): their own waits and watches, which
// Stop drops
class ScriptProcess {
public:
    void wait(int frames, std::function<void()> fn);                    // TimerImpl::Wait
    void when(std::function<bool()> test, std::function<void()> fn);    // polled every frame until true
    void stop();                                                        // cScriptProcessBase::Stop
    void tick();                                                        // once per 30 Hz frame
private:
    struct Wait { int frames; std::function<void()> fn; };
    struct When { std::function<bool()> test; std::function<void()> fn; };
    std::vector<Wait> waits_;
    std::vector<When> whens_;
    unsigned generation_ = 0;
};

class Mission {
public:
    virtual ~Mission() = default;
    virtual void start(Game& game) = 0;
    void tick(Game& game);                 // the waits and conditions that are due, once per 30 Hz frame
    bool finished() const { return finished_; }
    // InitMissionText("KEN_A01"): e_ken_a01.gxt; DisplayObjective ids from 0x52F on are its strings
    bool loadText(const std::string& dataDir, const std::string& name);
    std::u16string text(int id) const;

    // The mission's end state, as cActStructure keeps it: a mission runs until a script passes or fails it.
    enum Outcome { kRunning, kPassed, kFailed };
    Outcome outcome() const { return outcome_; }
    // The cash actually granted at the last pass (ScriptPlayer::GiveMoney in cActStructure::DoRewardForMission): the
    // mission's reward unless it was a replay, when the reward is nothing.
    int32_t rewardCash() const { return rewardCash_; }
    // The countdown clock a timed mission arms: its remaining 30 Hz frames, or -1 when no clock is running.
    int timerRemaining() const { return timerFrames_; }
    bool timerActive() const { return timerFrames_ >= 0; }
    // A multi-objective mission's bookkeeping (e.g. "collect N of X"): the goals set and the ones done so far.
    int objectivesTotal() const { return objTotal_; }
    int objectivesDone() const { return objDone_; }
    bool allObjectivesDone() const { return objTotal_ > 0 && objDone_ >= objTotal_; }

    // HUDImpl::DisplayWinLose / E_MAIN ids and times the mission end uses, from cActStructure::PassMission's path
    // (0x5D "MISSION PASSED" for 0x5A frames; 0x5C "MISSION FAILED" for 0x46) and cActStructure::DoRewardForMission
    // (0xcb8948: 0x5E the reward line and 0x5D together in font 5 for 0x32 frames, then GiveMoney).
    static constexpr int kPassStringId = 0x5D;
    static constexpr int kFailStringId = 0x5C;
    static constexpr int kRewardStringId = 0x5E;
    static constexpr int kPassedDisplayFrames = 0x5A;   // 90
    static constexpr int kFailedDisplayFrames = 0x46;   // 70
    static constexpr int kRewardDisplayFrames = 0x32;   // 50
    static constexpr int kWinLoseFont = 5;

protected:
    virtual void onFrame(Game&) {}                          // the mission's own processes, before its waits
    void wait(int frames, std::function<void()> fn);        // TimerImpl::Wait
    void when(std::function<bool()> test, std::function<void()> fn);   // polled every frame until true
    void cancelAll();                                       // cScriptProcessBase::Stop
    void finish() { finished_ = true; stopTimer(); cancelAll(); }

    // A mission time limit: `frames` 30 Hz frames down to zero, then onExpire once (TimerImpl::Wait style). tick()
    // counts it with the frame; the clock is dropped when the mission finishes.
    void startTimer(int frames, std::function<void()> onExpire = nullptr) {
        timerFrames_ = frames; onTimer_ = std::move(onExpire);
    }
    void stopTimer() { timerFrames_ = -1; onTimer_ = nullptr; }

    void setObjectiveCount(int n) { objTotal_ = n; objDone_ = 0; }   // the goals a mission tracks
    void completeObjective() { if (objDone_ < objTotal_) ++objDone_; }

    // cActStructure::DoRewardForMission (0xcb8948): a replay earns nothing (WorldImpl::IsOnReplayMission); otherwise
    // the per-mission cash is the reward, and the mission passes. Returns the cash to hand to ScriptPlayer::GiveMoney.
    int32_t passWithReward(int32_t cash, bool replay = false) {
        rewardCash_ = replay ? 0 : cash;
        outcome_ = kPassed;
        finish();
        return rewardCash_;
    }
    void pass() { outcome_ = kPassed; finish(); }    // cActStructure::PassMission (no reward to show)
    void fail() { outcome_ = kFailed; finish(); }    // cActStructure::FailCurrentMission

private:
    struct Wait { int frames; std::function<void()> fn; };
    struct When { std::function<bool()> test; std::function<void()> fn; };
    std::vector<Wait> waits_;
    std::vector<When> whens_;
    std::vector<std::u16string> text_;
    bool finished_ = false;
    unsigned generation_ = 0;   // bumps when everything is cancelled, so a running callback's queue is dropped
    Outcome outcome_ = kRunning;
    int32_t rewardCash_ = 0;
    int timerFrames_ = -1;      // the countdown clock; -1 when none is running
    std::function<void()> onTimer_;
    int objTotal_ = 0, objDone_ = 0;
};

// cScreenFade (CameraImpl::FadeToBlack / FadeIn): black over the picture, with a callback when it is done. The
// level is 0 (clear) .. 0x10000 (black). FadeOut steps (0x10000 - level) / frames a frame, FadeIn -level / frames
// (all of it when that rounds to 0); Process adds the step and stops at the end, or a step early when the next
// would pass it (a fade-in once the level is below 0x1000).
struct ScreenFade {
    float alpha = 0.f;   // level / 0x10000, for drawing
    bool white = false;  // the flash bang's daze fades white (cPlayer::Daze's FadeIn, b=0); every other fade is black
    void toBlack(int frames, std::function<void()> done = nullptr);   // cScreenFade::FadeOut
    void in(int frames, std::function<void()> done = nullptr);        // cScreenFade::FadeIn
    void tick();                                                      // cScreenFade::Process
    bool fading() const { return step_ != 0; }
    int32_t level() const { return level_; }
private:
    int32_t level_ = 0, step_ = 0, target_ = 0;   // +8, +0xC, +0x16 (16 or 0)
    std::function<void()> done_;
};

// HUDImpl::AddBlip for the mission: a radar marker (the original's eMarkerRepresentation sprites are drawn as a dot
// in the blip's colour, clamped to the radar's edge when out of range) and, for some representations, a marker in
// the world (markers.h)
struct RadarBlip {
    int32_t pos[3];
    uint32_t colour;   // 0xRRGGBBAA
    int id;
    char label[3]{};   // optional short plugin marker, e.g. A or AA; empty keeps the normal dot
    int8_t marker = -1;          // eMarkerRepresentation: 1 sMarker3D, 5 sMarkerChevron; -1 (a plugin's), 4 radar only
    uint8_t markerColour = 2;    // Location::SetColour (HUDImpl::AddBlip sets 2)
    int16_t markerAngle = 0;     // sMarkerChevron::SetAngle (Marker::SetAngle: degrees x 0xB6)
    uint8_t chevronType = 0;     // Marker::SetChevronType
    int32_t markerSize = 0;      // sPosPrimitive +0x70 (Marker::SetRadius); 0: the representation's own
    bool onRadar = true;         // Marker::SetBlip
    // the radar sprite (iPosition): -1 a plugin's dot; HUDImpl::AddBlip's sprite 106 in palette 7, long range
    int16_t sprite = -1;         // +0x46 (HUDImpl::SetBlipSprite)
    uint8_t palette = 7;         // +0x48 (Marker::SetSpritePalette)
    bool longRange = true;       // +0x4A bit 1 (eBlipRange 1): kept at the radar's edge when far
    int16_t appear = 0x1000;     // +0x40: the pop-in (DrawBlip / AnimScaleBlip), at the scale once it has settled
    int16_t spriteScale = 0x1000;   // the sprite's current scale
};


// the missions the port can start (kena01.cpp, ...)
std::unique_ptr<Mission> Mission_Create(const std::string& name);
