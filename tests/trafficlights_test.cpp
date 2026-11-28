// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Junction signal cycling, checked against cTrafficLightManager in libGame.so:
//   Flow                 0x916ba4  (frame & 0x3FF -> 0 / AllRed / 1 / AllRed over 1024 frames)
//   Direction            0x916ac4 / 0x916ae0  (|dx| >= |dy| ? 1 : 0)
//   IsTrafficBeingHeld   0x916b04 / 0x916b64  (held unless flow == approach direction)
//   Init                 0x916ab8  (mode = Cycling = 4)
//
// Expected values are re-derived from Flow's phase boundaries, not copied: the two green phases are 0x180 = 384
// frames (f in [0,0x180) flow 0; f in [0x200,0x380) flow 1) and the two all-red phases 0x80 = 128 frames
// (f in [0x180,0x200) and [0x380,0x400)), summing to 0x400 = 1024.
#include "trafficlights.h"
#include <cstdio>

struct TrafficLightTestAccess {
    static void setMode(TrafficLightManager& m, int v) { m.mode_ = v; }
};

using TL = TrafficLightManager;

static int failures;
static void check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; } }

// The flow expected at a raw (unmasked) frame, derived straight from the four phase windows.
static int expectedFlow(uint32_t frame) {
    const uint32_t f = frame & 0x3FFu;
    if (f < 0x180u) return TL::Dir0;          // 384 frames
    if (f < 0x200u) return TL::AllRed;        // 128 frames
    if (f < 0x380u) return TL::Dir1;          // 384 frames
    return TL::AllRed;                        // 128 frames
}

int main() {
    const TL lights;   // Init default: Cycling

    check(lights.mode() == TL::Cycling, "Init leaves the manager cycling (mode 4)");

    // --- Flow: exact phase boundaries ---
    check(lights.flow(0) == TL::Dir0, "f=0 -> direction 0 green");
    check(lights.flow(0x17F) == TL::Dir0, "f=383 (last of green 0)");
    check(lights.flow(0x180) == TL::AllRed, "f=384 -> all red");
    check(lights.flow(0x1FF) == TL::AllRed, "f=511 (last of first all-red)");
    check(lights.flow(0x200) == TL::Dir1, "f=512 -> direction 1 green");
    check(lights.flow(0x37F) == TL::Dir1, "f=895 (last of green 1)");
    check(lights.flow(0x380) == TL::AllRed, "f=896 -> all red");
    check(lights.flow(0x3FF) == TL::AllRed, "f=1023 (last of cycle)");
    check(lights.flow(0x400) == TL::Dir0, "f=1024 wraps back to green 0");

    // --- Flow over the whole cycle, and phase durations counted from it ---
    int dur0 = 0, durRed = 0, dur1 = 0;
    for (uint32_t f = 0; f < 0x400; ++f) {
        const int got = lights.flow(f);
        check(got == expectedFlow(f), "flow matches the derived phase window");
        if (got == TL::Dir0) ++dur0;
        else if (got == TL::Dir1) ++dur1;
        else ++durRed;
    }
    check(dur0 == 384, "direction-0 green lasts 384 frames");
    check(dur1 == 384, "direction-1 green lasts 384 frames");
    check(durRed == 256, "both all-red clearances total 256 frames (128 each)");
    check(dur0 + dur1 + durRed == 1024, "the four phases fill the 1024-frame cycle");

    // phase order: green0 -> red -> green1 -> red, each contiguous
    check(lights.flow(0x180 - 1) == TL::Dir0 && lights.flow(0x180) == TL::AllRed, "green0 then all-red");
    check(lights.flow(0x200 - 1) == TL::AllRed && lights.flow(0x200) == TL::Dir1, "all-red then green1");
    check(lights.flow(0x380 - 1) == TL::Dir1 && lights.flow(0x380) == TL::AllRed, "green1 then all-red");

    // --- Direction: approach axis from the link vector ---
    {
        const int32_t nodeNS[2] = {0, 10 << 12}, here[2] = {0, 0};   // link runs along +y
        check(TL::direction(nodeNS, here) == TL::Dir0, "a north-south link is direction 0");
        const int32_t nodeEW[2] = {10 << 12, 0};                     // link runs along +x
        check(TL::direction(nodeEW, here) == TL::Dir1, "an east-west link is direction 1");
        check(TL::direction(5, 5) == TL::Dir1, "|dx|==|dy| rounds to direction 1");
        check(TL::direction(3, -9) == TL::Dir0, "mostly-y vector is direction 0");
        // point order is irrelevant (the original takes the magnitude of the difference)
        const int32_t a[2] = {4 << 12, 1 << 12}, b[2] = {1 << 12, 4 << 12};
        check(TL::direction(a, b) == TL::direction(b, a), "direction is symmetric in its two points");
    }

    // --- IsTrafficBeingHeld: an approach goes only while its own axis is flowing ---
    {
        const int32_t ewFrom[2] = {10 << 12, 0}, ewTo[2] = {0, 0};   // direction 1
        const int32_t nsFrom[2] = {0, 10 << 12}, nsTo[2] = {0, 0};   // direction 0
        // green 0: NS goes, EW held
        check(!lights.isTrafficBeingHeld(0x000, nsFrom, nsTo), "NS not held during green 0");
        check(lights.isTrafficBeingHeld(0x000, ewFrom, ewTo), "EW held during green 0");
        // green 1: EW goes, NS held
        check(!lights.isTrafficBeingHeld(0x200, ewFrom, ewTo), "EW not held during green 1");
        check(lights.isTrafficBeingHeld(0x200, nsFrom, nsTo), "NS held during green 1");
        // all-red: both held
        check(lights.isTrafficBeingHeld(0x180, nsFrom, nsTo), "NS held during all-red");
        check(lights.isTrafficBeingHeld(0x180, ewFrom, ewTo), "EW held during all-red");
        // the eDirection overload agrees with the two-point overload
        check(lights.isTrafficBeingHeld(0x000, TL::Dir1) == lights.isTrafficBeingHeld(0x000, ewFrom, ewTo),
              "direction and two-point overloads agree");
    }

    // --- forced mode (scripted override): Flow returns mode_ verbatim unless Cycling ---
    {
        TL forced;
        TrafficLightTestAccess::setMode(forced, TL::Dir1);
        for (uint32_t f = 0; f < 0x400; f += 0x37) check(forced.flow(f) == TL::Dir1, "forced flow ignores the timer");
        TrafficLightTestAccess::setMode(forced, TL::AllRed);
        check(forced.flow(0) == TL::AllRed && forced.flow(0x200) == TL::AllRed, "forced all-red holds every phase");
        forced.init();   // back to cycling
        check(forced.flow(0x200) == TL::Dir1, "init restores the automatic cycle");
    }

    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("trafficlights: all checks passed\n");
    return 0;
}
