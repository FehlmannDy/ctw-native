// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// cMagneticCraneApp control logic and arm kinematics. See crane.h for the routine
// map and cited addresses. This translation unit has no engine dependencies: the
// sprite/vehicle/cable glue is reported as hooks, not implemented here.
//
// Reported cross-file hooks (NOT made here, no dead code written):
//   * The five directional/magnet callbacks (fireDirection / magnet toggle) are
//     the mission script's ScriptPlayer::SetMagneticCrane{Left,Center,Right,
//     MagnetOn,MagnetOff}Callback handlers (0x008b7fd0..0x008b82b0). The host must
//     forward MagneticCrane::Event into cCallBack::Call. Observation.
//   * The magnet attach/detach that actually grabs/drops the held vehicle happens
//     inside those MagnetOn/MagnetOff script handlers and in the magnet entity;
//     the vehicle lives in off-limits vehicle.*/game.*. The port carries the
//     control state (magnetOn + direction) that drives them. Observation.
//   * cCraneCable::Callback_DoCable (0x00bfe0b8) draws the cable each tick:
//     WorldImpl::DrawLine from (arm entity + kCableOffset) to the magnet entity
//     origin, rescheduling itself via TimerImpl::Wait(1). Render-only; left as a
//     hook. kCableOffset{0,-3,5} is verified (crane.h). Observation.
//   * Sprite frame/position updates (cSprite::SetSpriteData/SetSpritePos) consume
//     frameForColumn() and handleX(); the sprite objects are renderer-owned.

#include "crane.h"

namespace ctw {
namespace crane {

MagneticCrane::MagneticCrane() = default;

void MagneticCrane::fireDirection(Dir d, EventLog& out) {
    // Fire the directional callback only on a zone change, matching the binary's
    // transition gating on field 0x211 (Left requires the prior dir to be Center,
    // 0x0080e654). The real Center handler fires every in-band frame; the port
    // fires on transition for a self-consistent event stream. Assumption.
    if (d == dir_) return;
    dir_ = d;
    switch (d) {
        case Dir::Left:   out.push(Event::Left);   break;
        case Dir::Center: out.push(Event::Center); break;
        case Dir::Right:  out.push(Event::Right);  break;
    }
}

// cMagneticCraneApp::OnDrag (0x0080e5f4)
int MagneticCrane::onDrag(int dragX, EventLog& out) {
    requestedSfx_ = -1;

    // newColumn = column + (dragX - lastDragX)  (0x0080e630..0x0080e634)
    const int delta  = dragX - lastDragX_;
    const int newCol = column_ + delta;

    // Gate to [kColumnMin, kColumnMax]; outside, the column is not moved but the
    // last drag X is still recorded (0x0080e638 b.hi -> 0x0080e804).
    if (newCol >= kColumnMin && newCol <= kColumnMax) {
        column_ = newCol;

        // Zone -> directional callback (0x0080e644 / 0x0080e6a8 / 0x0080e6fc).
        fireDirection(zoneForColumn(newCol), out);

        // Edge "click" when the stick first enters a side band
        // (0x0080e794..0x0080e800), sfx kSfxMoveClick, volume 127.
        if (newCol >= kClickBandLo) {
            leftLatch_ = false;
            if (newCol <= kClickBandHi) {
                rightLatch_ = false;            // still in the centre band
            } else if (!rightLatch_) {          // entered the right band
                rightLatch_ = true;
                requestedSfx_ = kSfxMoveClick;
            }
        } else {                                // entered the left band
            rightLatch_ = false;
            if (!leftLatch_) {
                leftLatch_ = true;
                requestedSfx_ = kSfxMoveClick;
            }
        }
    }

    lastDragX_ = dragX; // store crane_lastPosX (0x0080e814)
    return column_;
}

// cMagneticCraneApp::OnPress (0x0080e2fc)
bool MagneticCrane::onPress(int x, int y, EventLog& out) {
    requestedSfx_ = -1;
    if (!isOverButton(x, y)) return false; // miss -> cWnd::OnPress, not handled

    if (magnetOn_) {
        magnetOn_ = false;
        out.push(Event::MagnetOff);
    } else {
        magnetOn_ = true;
        out.push(Event::MagnetOn);
    }
    // cinc w1, 0x24c, ne (0x0080e468): now-on -> 0x24d, now-off -> 0x24c.
    requestedSfx_ = magnetOn_ ? kSfxMagnetOn : kSfxMagnetOff;
    return true;
}

// cMagneticCraneApp::Process (0x0080dd64), analog-pad path.
int MagneticCrane::process(float analog, bool buttonPressed, EventLog& out) {
    requestedSfx_ = -1;

    // Commit a direction past the deadzone (|analog| >= 0.88) and snap the arm
    // column to that extreme; inside the deadzone, centre it (0x0080df48..).
    if (analog <= -kAnalogThreshold) {
        column_ = kColumnMin;
        fireDirection(Dir::Left, out);
        handleX_ = column_;            // stick follows the committed side
    } else if (analog >= kAnalogThreshold) {
        column_ = kColumnMax;
        fireDirection(Dir::Right, out);
        handleX_ = column_;
    } else {
        column_ = kColumnCenter;
        fireDirection(Dir::Center, out);
        // Idle: the joystick handle drifts back to centre at kRecenterSpeed,
        // snapping when within a step (0x0080ddf4..0x0080de48).
        if (handleX_ < kColumnCenter) {
            handleX_ = (handleX_ + kRecenterSpeed > kColumnCenter)
                           ? kColumnCenter : handleX_ + kRecenterSpeed;
        } else if (handleX_ > kColumnCenter) {
            handleX_ = (handleX_ - kRecenterSpeed < kColumnCenter)
                           ? kColumnCenter : handleX_ - kRecenterSpeed;
        }
    }

    // Button edge toggles the magnet, same as OnPress (0x0080e064..0x0080e1e4).
    if (buttonPressed) {
        if (magnetOn_) { magnetOn_ = false; out.push(Event::MagnetOff); }
        else           { magnetOn_ = true;  out.push(Event::MagnetOn);  }
        requestedSfx_ = magnetOn_ ? kSfxMagnetOn : kSfxMagnetOff;
    }

    // Keep the loop-sfx channel state coherent (the loop id itself is surfaced by
    // processMagnetLoop()); mirrors the bookkeeping at 0x0080dd80.
    loopPlaying_ = magnetOn_;

    return frameForColumn(column_);
}

// cMagneticCraneApp::ProcessMagnetLoop (0x0080e1fc)
bool MagneticCrane::processMagnetLoop() {
    requestedSfx_ = -1;
    if (magnetOn_) {
        if (!loopPlaying_) {           // channel was 0xff -> start the loop
            loopPlaying_ = true;
            requestedSfx_ = kSfxJoystickLoop; // id 0x14f, volume 30
        }
    } else if (loopPlaying_) {         // channel live -> StopChannel, mark 0xff
        loopPlaying_ = false;
    }
    return loopPlaying_;
}

// cMagneticCraneApp::IsOverButton (0x0080e498): box x in [841,953], y in [638,750].
bool MagneticCrane::isOverButton(int x, int y) {
    const unsigned dx = static_cast<unsigned>(x - kButtonX0);
    const unsigned dy = static_cast<unsigned>(y - kButtonY0);
    return dx <= static_cast<unsigned>(kButtonSpan) &&
           dy <= static_cast<unsigned>(kButtonSpan);
}

// cMagneticCraneApp::IsHoldingJoystick (0x0080e538):
// x in [handleX + 30, handleX + width + 40].
bool MagneticCrane::isHoldingJoystick(int x, int handleX, int handleWidth) {
    if (x < handleX + kHandleMarginLeft) return false;         // 0x0080e550 b.hi
    return handleX + handleWidth + kHandleMarginRight >= x;    // 0x0080e568 b.hs
}

// Arm sprite frame from the column (Process 0x0080dea8..0x0080df18).
Frame MagneticCrane::frameForColumn(int column) {
    if (column >= kFrameFarRightMin) return FrameFarRight; // >= 624
    if (column >= kFrameRightMin)    return FrameRight;    // 615..623
    if (column >= kFrameCenterMin)   return FrameCenter;   // 608..614
    if (column <= kFrameFarLeftMax)  return FrameFarLeft;  // <= 598
    return FrameLeft;                                      // 599..607
}

// Directional zone from the column (OnDrag).
Dir MagneticCrane::zoneForColumn(int column) {
    if (column <= kLeftZoneMax)  return Dir::Left;   // <= 601
    if (column >= kRightZoneMin) return Dir::Right;  // >= 621
    return Dir::Center;                              // 602..620
}

} // namespace crane
} // namespace ctw
