// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
// Junction signal cycling: a port of cTrafficLightManager (0x916ab8..0x916c78).
//
// A single global manager drives every controlled junction off one game timer (the original reads a global frame
// counter at 0xdce718; the port passes the game frame, as aispline's holding-pattern code already did). Flow() turns
// the low 10 bits of that counter into which approach is "go": the cycle repeats every 1024 frames.
//
//   frame & 0x3FF   flow   meaning
//   [0x000,0x180)    0     direction-0 approaches go        (384 frames)
//   [0x180,0x200)    3     all red (clearance)              (128 frames)
//   [0x200,0x380)    1     direction-1 approaches go        (384 frames)
//   [0x380,0x400)    3     all red (clearance)              (128 frames)
//
// (Observation from Flow at 0x916ba4: the two green phases are 0x180 = 384 frames each and the two all-red clearance
// phases 0x80 = 128 frames each; the original has no separate amber flow value - amber is folded into the all-red
// clearance window. At the game's 30 fps the whole cycle is ~34.1 s: ~12.8 s green per axis, ~4.3 s all-red between.)
//
// eDirection of an approach is which axis it runs along: Direction(v) = |v.x| >= |v.y| ? 1 : 0 (0x916ac4/0x916ae0),
// so direction 1 is the more east-west link, direction 0 the more north-south. Traffic on an approach is held
// whenever Flow() does not equal that approach's direction (IsTrafficBeingHeld, 0x916b04/0x916b64) - i.e. the other
// axis is green, or it is an all-red phase (flow 3 never equals 0 or 1, so both axes are held).
//
// mode_ is the manager's only state (Init at 0x916ab8 sets it to Cycling=4). Flow returns mode_ verbatim unless it is
// Cycling, which lets a junction be forced to a fixed flow (0, 1 or AllRed). The port never forces it, so mode_ stays
// Cycling; the field is kept to match the original exactly.
#pragma once
#include <cstdint>
#include <cstdlib>

class TrafficLightManager {
public:
    enum eDirection { Dir0 = 0, Dir1 = 1, AllRed = 3, Cycling = 4 };

    void init() { mode_ = Cycling; }   // cTrafficLightManager::Init (0x916ab8)

    // cTrafficLightManager::Flow (0x916ba4): the currently-flowing approach direction, or AllRed, from the timer.
    int flow(uint32_t timer) const {
        if (mode_ != Cycling) return mode_;
        const uint32_t f = timer & 0x3FFu;
        if (f < 0x180u) return Dir0;
        return (f - 0x200u) < 0x180u ? Dir1 : AllRed;   // unsigned: [0x180,0x200) wraps high -> AllRed
    }

    // cTrafficLightManager::Direction (0x916ac4 vector form; 0x916ae0 two-point form). The approach axis: 1 if the
    // link is more east-west than north-south, else 0. Abs makes the point order irrelevant.
    static int direction(int32_t dx, int32_t dy) { return std::abs(dx) >= std::abs(dy) ? Dir1 : Dir0; }
    static int direction(const int32_t a[2], const int32_t b[2]) { return direction(a[0] - b[0], a[1] - b[1]); }

    // cTrafficLightManager::IsTrafficBeingHeld (0x916b64 direction form; 0x916b04 two-point form): held unless the
    // approach's direction is the one flowing now.
    bool isTrafficBeingHeld(uint32_t timer, int dir) const { return flow(timer) != dir; }
    bool isTrafficBeingHeld(uint32_t timer, const int32_t from[2], const int32_t to[2]) const {
        return isTrafficBeingHeld(timer, direction(from, to));
    }

    int mode() const { return mode_; }

private:
    int mode_ = Cycling;   // this[0]
    friend struct TrafficLightTestAccess;
};
