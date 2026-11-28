// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// cMagneticCraneApp control + arm kinematics, checked against libGame.so.
// Every expected value below is re-derived by hand from the boundaries documented
// in crane.h (columns 590/611/632, zone edges 601/602/620/621, deadzone 0.88,
// recenter 4 px/frame, button box [841,953]x[638,750], sfx ids), never copied
// from the implementation.
#include "crane.h"
#include <cstdio>

using namespace ctw::crane;

static int failures;
static void check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; }
}

static void test_initial_pose() {
    MagneticCrane c;
    check(!c.magnetOn(), "ctor: magnet starts off");
    check(c.direction() == Dir::Center, "ctor: arm centred");
    check(c.column() == 611, "ctor: column at centre 611");
    check(c.handleX() == 611, "ctor: handle at centre 611");
    check(!c.loopPlaying(), "ctor: magnet loop silent");
}

static void test_frame_buckets() {
    // 5 frames split at 598 / 607 / 614 / 623.
    check(MagneticCrane::frameForColumn(590) == FrameFarLeft, "590 -> far-left frame");
    check(MagneticCrane::frameForColumn(598) == FrameFarLeft, "598 last far-left");
    check(MagneticCrane::frameForColumn(599) == FrameLeft,    "599 first left");
    check(MagneticCrane::frameForColumn(607) == FrameLeft,    "607 last left");
    check(MagneticCrane::frameForColumn(608) == FrameCenter,  "608 first centre");
    check(MagneticCrane::frameForColumn(614) == FrameCenter,  "614 last centre");
    check(MagneticCrane::frameForColumn(615) == FrameRight,   "615 first right");
    check(MagneticCrane::frameForColumn(623) == FrameRight,   "623 last right");
    check(MagneticCrane::frameForColumn(624) == FrameFarRight,"624 first far-right");
    check(MagneticCrane::frameForColumn(632) == FrameFarRight,"632 far-right");
}

static void test_zones() {
    check(MagneticCrane::zoneForColumn(590) == Dir::Left,   "590 -> left zone");
    check(MagneticCrane::zoneForColumn(601) == Dir::Left,   "601 last left zone");
    check(MagneticCrane::zoneForColumn(602) == Dir::Center, "602 first centre zone");
    check(MagneticCrane::zoneForColumn(620) == Dir::Center, "620 last centre zone");
    check(MagneticCrane::zoneForColumn(621) == Dir::Right,  "621 first right zone");
    check(MagneticCrane::zoneForColumn(632) == Dir::Right,  "632 right zone");
}

static void test_button_box() {
    check(MagneticCrane::isOverButton(841, 638), "top-left corner inside");
    check(MagneticCrane::isOverButton(953, 750), "bottom-right corner inside");
    check(MagneticCrane::isOverButton(897, 694), "centre of box inside");
    check(!MagneticCrane::isOverButton(840, 694), "one px left of box");
    check(!MagneticCrane::isOverButton(954, 694), "one px right of box");
    check(!MagneticCrane::isOverButton(897, 637), "one px above box");
    check(!MagneticCrane::isOverButton(897, 751), "one px below box");
}

static void test_joystick_hit() {
    // handle sprite at x=611, width=20 -> hit window [641, 671].
    const int hx = 611, w = 20;
    check(!MagneticCrane::isHoldingJoystick(640, hx, w), "640 left of handle window");
    check(MagneticCrane::isHoldingJoystick(641, hx, w),  "641 = hx+30 inside");
    check(MagneticCrane::isHoldingJoystick(671, hx, w),  "671 = hx+w+40 inside");
    check(!MagneticCrane::isHoldingJoystick(672, hx, w), "672 right of window");
}

static void test_drag_kinematics() {
    MagneticCrane c;              // column 611, lastDragX 611, dir Center
    EventLog ev;

    // Drag right to 621: delta +10 -> column 621 (right zone), fires Right,
    // crosses into the right band (>620) -> one move-click.
    ev.clear();
    check(c.onDrag(621, ev) == 621, "drag to 621 lands at 621");
    check(c.direction() == Dir::Right, "621 -> right");
    check(ev.contains(Event::Right), "entering right fires Right callback");
    check(c.requestedSfx() == kSfxMoveClick, "entering right band clicks (0x24e)");

    // Drag left to 601: delta -20 -> column 601 (left zone), fires Left, enters
    // the left band (<602) -> one click.
    ev.clear();
    check(c.onDrag(601, ev) == 601, "drag to 601 lands at 601");
    check(c.direction() == Dir::Left, "601 -> left");
    check(ev.contains(Event::Left), "entering left fires Left callback");
    check(c.requestedSfx() == kSfxMoveClick, "entering left band clicks");

    // Drag back to 611 (centre band): delta +10 -> column 611, fires Center, no
    // click (returning to the centre band is silent).
    ev.clear();
    check(c.onDrag(611, ev) == 611, "drag to 611 lands at 611");
    check(c.direction() == Dir::Center, "611 -> centre");
    check(ev.contains(Event::Center), "returning to centre fires Center callback");
    check(c.requestedSfx() == -1, "centre band is silent");

    // Over-travel: from column 611, lastDragX 611, a +50 drag targets 661 (>632)
    // so the column is pinned and nothing fires.
    ev.clear();
    check(c.onDrag(661, ev) == 611, "drag past max is clamped (column unchanged)");
    check(ev.count == 0, "clamped drag fires nothing");
    check(c.requestedSfx() == -1, "clamped drag is silent");
}

static void test_press_toggle() {
    MagneticCrane c;
    EventLog ev;

    // Miss outside the box: not handled, nothing changes.
    ev.clear();
    check(!c.onPress(10, 10, ev), "press outside box not handled");
    check(ev.count == 0 && !c.magnetOn(), "miss leaves magnet off");

    // First valid press: off -> on, MagnetOn, sfx 0x24d.
    ev.clear();
    check(c.onPress(897, 694, ev), "press inside box handled");
    check(c.magnetOn(), "magnet now on");
    check(ev.contains(Event::MagnetOn), "fires MagnetOn");
    check(c.requestedSfx() == kSfxMagnetOn, "magnet-on sfx 0x24d");

    // Second press: on -> off, MagnetOff, sfx 0x24c.
    ev.clear();
    check(c.onPress(897, 694, ev), "second press handled");
    check(!c.magnetOn(), "magnet now off");
    check(ev.contains(Event::MagnetOff), "fires MagnetOff");
    check(c.requestedSfx() == kSfxMagnetOff, "magnet-off sfx 0x24c");
}

static void test_analog_process() {
    MagneticCrane c;              // column 611, dir Center, handle 611
    EventLog ev;

    // Full left past the deadzone: column snaps to 590, fires Left, far-left frame.
    ev.clear();
    check(c.process(-1.0f, false, ev) == FrameFarLeft, "analog -1 -> far-left frame");
    check(c.column() == 590, "analog left snaps column to 590");
    check(c.direction() == Dir::Left, "analog left -> Left");
    check(ev.contains(Event::Left), "analog left fires Left");
    check(c.handleX() == 590, "handle follows to 590 while pushed");

    // Full right: column 632, Right, far-right frame.
    ev.clear();
    check(c.process(1.0f, false, ev) == FrameFarRight, "analog +1 -> far-right frame");
    check(c.column() == 632, "analog right snaps column to 632");
    check(ev.contains(Event::Right), "analog right fires Right");

    // Release to the deadzone: column 611, Center, centre frame; the handle (at
    // 632) recenters by one 4-px step to 628 (not yet within a step of 611).
    ev.clear();
    check(c.process(0.1f, false, ev) == FrameCenter, "deadzone -> centre frame");
    check(c.column() == 611, "deadzone centres column");
    check(ev.contains(Event::Center), "release fires Center");
    check(c.handleX() == 628, "handle steps 632 -> 628 (one 4-px step)");

    // Hold the deadzone until the handle snaps home: 628 -> 624 -> 620 -> 616 ->
    // 612 -> 611 (612-4=608 < 611 so it snaps). Five more steps.
    for (int i = 0; i < 5; ++i) { ev.clear(); c.process(0.0f, false, ev); }
    check(c.handleX() == 611, "handle recenters to 611");

    // A button edge during process toggles the magnet (off -> on) with sfx 0x24d.
    ev.clear();
    check(c.process(0.0f, true, ev) >= 0, "process runs with a button edge");
    check(c.magnetOn(), "button edge turns magnet on");
    check(ev.contains(Event::MagnetOn), "button edge fires MagnetOn");
    check(c.requestedSfx() == kSfxMagnetOn, "button edge plays magnet-on sfx");
}

static void test_magnet_loop() {
    MagneticCrane c;
    EventLog ev;

    // Magnet off -> loop silent, no sfx.
    check(!c.processMagnetLoop(), "loop silent while magnet off");
    check(c.requestedSfx() == -1, "no loop sfx while off");

    // Turn the magnet on via a press (press does not touch the loop channel).
    ev.clear();
    c.onPress(897, 694, ev);
    check(c.magnetOn(), "magnet on for loop test");

    // First loop tick after switch-on starts the loop (sfx 0x14f).
    check(c.processMagnetLoop(), "loop audible once magnet on");
    check(c.requestedSfx() == kSfxJoystickLoop, "loop start plays 0x14f");

    // Subsequent ticks keep it playing without re-triggering the sfx.
    check(c.processMagnetLoop(), "loop stays audible");
    check(c.requestedSfx() == -1, "loop not re-triggered");

    // Switch off, then a loop tick stops it.
    ev.clear();
    c.onPress(897, 694, ev);
    check(!c.magnetOn(), "magnet off again");
    check(!c.processMagnetLoop(), "loop stops when magnet off");
    check(c.requestedSfx() == -1, "stopping the loop plays no sfx");
}

int main() {
    test_initial_pose();
    test_frame_buckets();
    test_zones();
    test_button_box();
    test_joystick_hit();
    test_drag_kinematics();
    test_press_toggle();
    test_analog_process();
    test_magnet_loop();

    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("all crane checks passed\n");
    return 0;
}
