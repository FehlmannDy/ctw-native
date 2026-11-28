// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// cMagneticCraneApp - the scrapyard magnetic-crane minigame control logic,
// matched routine-by-routine against libGame.so (Android 4.4.243):
//
//   cMagneticCraneApp::cMagneticCraneApp   0x0080d154  (initial pose/state)
//   cMagneticCraneApp::Process             0x0080dd64  (analog-pad step: threshold +/-0.88 -> dir, arm recenter @4px, frame pick)
//   cMagneticCraneApp::OnDrag              0x0080e5f4  (touch drag kinematics: column += delta, clamp, zone callbacks, edge click)
//   cMagneticCraneApp::OnPress             0x0080e2fc  (magnet-button box, toggle magnet, sfx)
//   cMagneticCraneApp::IsOverButton        0x0080e498  (magnet-button hit test)
//   cMagneticCraneApp::IsHoldingJoystick   0x0080e538  (joystick-handle hit test)
//   cMagneticCraneApp::ProcessMagnetLoop   0x0080e1fc  (magnet loop sfx bookkeeping)
//
// This file ports only the self-contained control logic and arm kinematics. The
// vehicle magnet attach/detach, the cable render (cCraneCable::Callback_DoCable
// 0x00bfe0b8) and the sprite rendering live in off-limits files and are left as
// reported hooks (see crane.cpp). The five directional/magnet callbacks fire the
// mission script's ScriptPlayer::SetMagneticCrane*Callback handlers; here they are
// recorded as events so the behaviour is observable and testable.

#ifndef CTW_GAME_CRANE_H
#define CTW_GAME_CRANE_H

#include <cstdint>

namespace ctw {
namespace crane {

// ---- Constants verified against libGame.so (all are screen-space pixel columns
//      unless noted). Addresses cited in crane.cpp. ----

// Horizontal travel of the crane arm. 0x0080e638/0x0080e63c gate the column to
// [COLUMN_MIN, COLUMN_MAX]; the centred rest column is COLUMN_CENTER.
constexpr int kColumnMin    = 0x24e; // 590
constexpr int kColumnCenter = 0x263; // 611
constexpr int kColumnMax    = 0x278; // 632

// Zone classification used by OnDrag to fire the directional callbacks
// (0x0080e644 / 0x0080e6a8 / 0x0080e6fc).
constexpr int kLeftZoneMax   = 0x259; // <= 601  -> Left
constexpr int kCenterZoneLo  = 0x25a; //  602..620 -> Center
constexpr int kCenterZoneHi  = 0x26c; //  (inclusive)
constexpr int kRightZoneMin  = 0x26d; // >= 621  -> Right

// Move-click ("entered a side band") edge boundaries (0x0080e794 / 0x0080e79c).
constexpr int kClickBandLo = 0x25a; // 602
constexpr int kClickBandHi = 0x26c; // 620

// Analog-pad deadzone threshold, |analog| >= 0.88 commits a direction
// (float consts 0xBF6147AE / 0x3F6147AE at 0x0080df5c / 0x0080df74).
constexpr float kAnalogThreshold = 0.88f;

// Joystick-handle recenter: when idle the handle sprite drifts back to
// COLUMN_CENTER at this many pixels per frame (0x0080de10 / 0x0080de30).
constexpr int kRecenterSpeed = 4;

// Arm sprite frame picked from the column (0x0080dea8..0x0080df18), 5 frames.
constexpr int kFrameFarLeftMax = 0x256; // <= 598 -> frame 0
constexpr int kFrameLeftMax    = 0x25f; // 599..607 -> frame 1 (default band)
constexpr int kFrameCenterMin  = 0x260; // 608..614 -> frame 2
constexpr int kFrameRightMin   = 0x267; // 615..623 -> frame 3
constexpr int kFrameFarRightMin= 0x270; // >= 624 -> frame 4

// Magnet button touch box (0x0080e310..0x0080e330): x in [841,953], y in [638,750].
constexpr int kButtonX0 = 0x349; // 841
constexpr int kButtonY0 = 0x27e; // 638
constexpr int kButtonSpan = 0x70; // inclusive upper bound offset (112)

// Joystick-handle hit-test margins (0x0080e54c / 0x0080e564).
constexpr int kHandleMarginLeft  = 0x1e; // 30
constexpr int kHandleMarginRight = 0x28; // 40

// Sound effect ids (cAudio::PlaySfx calls).
constexpr int kSfxJoystickLoop = 0x14f; // magnet-held loop, volume 30 (0x0080ddbc)
constexpr int kSfxMoveClick    = 0x24e; // crosses into a side band, volume 127 (0x0080e7e0)
constexpr int kSfxMagnetOff    = 0x24c; // magnet switched off, volume 127 (0x0080e460)
constexpr int kSfxMagnetOn     = 0x24d; // magnet switched on,  volume 127

// Cable attach offset on the crane-arm entity, local coords, cFixed<20,12>
// (0x00bfe0e0 const 0x00479b00 -> x=0.0, y=-3.0; z literal 0x5000 -> 5.0).
// Reported hook: cCraneCable draws a line from this point to the magnet origin.
constexpr float kCableOffsetX = 0.0f;
constexpr float kCableOffsetY = -3.0f;
constexpr float kCableOffsetZ = 5.0f;

// Raw direction-state values as stored at field 0x211 in the binary.
enum class Dir : std::uint16_t {
    Center = 0x000,
    Left   = 0x001,
    Right  = 0x100,
};

// The callbacks the crane fires toward the mission script, and the sfx it asks
// the audio engine to play - recorded so behaviour is observable/testable.
enum class Event : std::uint8_t {
    Left, Center, Right, MagnetOn, MagnetOff,
};

// Arm frame index returned by Process (which sprite column art to show).
enum Frame { FrameFarLeft = 0, FrameLeft, FrameCenter, FrameRight, FrameFarRight };

// A tiny fixed-capacity event log (no heap; the minigame fires at most a handful
// of callbacks per frame).
struct EventLog {
    static constexpr int kMax = 16;
    Event items[kMax];
    int count = 0;
    void push(Event e) { if (count < kMax) items[count++] = e; }
    void clear() { count = 0; }
    bool contains(Event e) const {
        for (int i = 0; i < count; ++i) if (items[i] == e) return true;
        return false;
    }
};

// Self-contained port of cMagneticCraneApp's control state. Sprite handles,
// vehicle attach and rendering are deliberately absent (reported hooks).
class MagneticCrane {
public:
    // cMagneticCraneApp::cMagneticCraneApp (0x0080d154)
    MagneticCrane();

    // cMagneticCraneApp::OnDrag (0x0080e5f4). dragX is the touch X this frame.
    // Returns the new arm column. Fills `out` with any callbacks fired and
    // requests move-click sfx via requestedSfx()/clearSfx().
    int onDrag(int dragX, EventLog& out);

    // cMagneticCraneApp::OnPress (0x0080e2fc). Returns true if (x,y) hit the
    // magnet button (and the magnet was toggled); fires MagnetOn/MagnetOff.
    bool onPress(int x, int y, EventLog& out);

    // cMagneticCraneApp::Process (0x0080dd64), analog-pad path. `analog` is the
    // stick's horizontal axis in [-1,1]; buttonPressed is an edge (just-pressed).
    // Commits a direction past the deadzone, toggles magnet on a button edge,
    // recenters the idle handle, and returns the arm frame for the current column.
    int process(float analog, bool buttonPressed, EventLog& out);

    // cMagneticCraneApp::ProcessMagnetLoop (0x0080e1fc). Keeps the magnet loop
    // sfx playing while the magnet is on; stops it when off. Returns whether the
    // loop should be audible this frame.
    bool processMagnetLoop();

    // cMagneticCraneApp::IsOverButton (0x0080e498).
    static bool isOverButton(int x, int y);

    // cMagneticCraneApp::IsHoldingJoystick (0x0080e538). handleX/handleWidth are
    // the live joystick sprite bounds (supplied by the renderer in the real game).
    static bool isHoldingJoystick(int x, int handleX, int handleWidth);

    // Maps an arm column to one of the 5 sprite frames (Process 0x0080dea8..).
    static Frame frameForColumn(int column);

    // Classifies a column into the directional zone used by OnDrag.
    static Dir zoneForColumn(int column);

    // --- observable state ---
    bool magnetOn() const { return magnetOn_; }
    Dir  direction() const { return dir_; }
    int  column() const { return column_; }
    int  handleX() const { return handleX_; }
    bool loopPlaying() const { return loopPlaying_; }
    int  requestedSfx() const { return requestedSfx_; }   // -1 if none this frame
    void clearSfx() { requestedSfx_ = -1; }

private:
    void fireDirection(Dir d, EventLog& out);

    bool magnetOn_   = false;   // field 0x215
    Dir  dir_        = Dir::Center; // field 0x211
    int  column_     = kColumnCenter; // field 0x21c
    int  handleX_    = kColumnCenter; // joystick handle sprite x (field 0x190 + 0x28)
    int  lastDragX_  = kColumnCenter; // global crane_lastPosX (0xdd05a0)
    bool leftLatch_  = false;   // field 0x214 (entered left band)
    bool rightLatch_ = false;   // field 0x213 (entered right band)
    bool loopPlaying_= false;   // derived from sfx channel field 0x218
    int  requestedSfx_ = -1;    // last PlaySfx id requested this call
};

} // namespace crane
} // namespace ctw

#endif // CTW_GAME_CRANE_H
