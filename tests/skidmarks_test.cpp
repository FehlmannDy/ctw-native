// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Single-player skid-mark math, checked against libGame.so:
//   cSkidmark::AddPoint   0x0089e9a4  (origin*16, half-width dir*0x19A0>>16, dz bias +0x4CC, *16 vertices, dedup)
//   cSkidmarkManager::Process / cSkidmark::Process  0x0089ef24 / 0x0089eafc  (hold -> wait -> alpha -> free)
//   bounds: dx,dy in [-0x8000,0x8000), dz in [-0x84CC,0x7B34)   (cmhi 0x10000 / (dz+0x84CC)>>16)
// Synthetic data only; no GL context, so render() is not exercised here.
#include "skidmarks.h"
#include <cstdio>
#include <memory>

struct SkidmarksTestAccess {
    using Mark = Skidmarks::Mark;
    static const Mark& mark(const Skidmarks& s, int i) { return s.marks_[i]; }
    // addPoint grabs a free slot by scanning for id==0 without breaking, so a new mark lands in the LAST free
    // slot, not slot 0. Which free slot is used has no behavioural meaning (slots are independent), so the test
    // locates a mark by its wheel id rather than assuming an index.
    static const Mark& markById(const Skidmarks& s, uint32_t id) {
        for (const auto& m : s.marks_) if (m.id == id) return m;
        static const Mark none{};
        return none;
    }
    static int usedSlots(const Skidmarks& s) { int n = 0; for (const auto& m : s.marks_) n += (m.id != 0); return n; }
    static int points(const Skidmarks& s) { int n = 0; for (const auto& m : s.marks_) n += m.count; return n; }
};

static int failures;
static void check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; } }

int main() {
    using TA = SkidmarksTestAccess;

    // --- geometry: first point lays origin and a left/right pair ---
    {
        auto sp = std::make_unique<Skidmarks>();
        Skidmarks& s = *sp;
        const int32_t p[3] = {1000, 2000, 3000};
        const int32_t dir[3] = {0x1000, 0, 0};   // unit +x, half-width = 0x1000*0x19A0>>16 = 0x19A
        check(s.addPoint(1, p, dir, 1), "first point accepted");
        const auto& m = TA::markById(s, 1);
        check(m.id == 1 && m.active && m.count == 2, "first point opens a 2-vertex strip");
        check(m.origin[0] == 16000 && m.origin[1] == 32000 && m.origin[2] == 48000, "origin is pos*16");
        // dx=dy=0, dz=0 then +0x4CC; wx=0x19A, wz=0. vertices stored *16.
        check(m.pts[0][0] == -0x19A * 16 && m.pts[0][1] == 0 && m.pts[0][2] == 0x4CC * 16, "left vertex = (dx-w, .., dz+bias)*16");
        check(m.pts[1][0] == 0x19A * 16 && m.pts[1][1] == 0 && m.pts[1][2] == 0x4CC * 16, "right vertex mirrors across the half-width");
    }

    // --- a second, distinct point grows the strip; an identical one is de-duplicated ---
    {
        auto sp = std::make_unique<Skidmarks>();
        Skidmarks& s = *sp;
        const int32_t dir[3] = {0x1000, 0, 0};
        int32_t p[3] = {1000, 2000, 3000};
        check(s.addPoint(1, p, dir, 1), "p0 accepted");
        p[0] = 1100;
        check(s.addPoint(1, p, dir, 1), "p1 (moved) accepted");
        check(TA::markById(s, 1).count == 4, "distinct point extends the strip by one pair");
        check(!s.addPoint(1, p, dir, 1), "repeating the same left vertex is rejected (dedup)");
        check(TA::markById(s, 1).count == 4, "dedup leaves the strip length unchanged");
    }

    // --- out-of-range point deactivates the strip instead of extending it ---
    {
        auto sp = std::make_unique<Skidmarks>();
        Skidmarks& s = *sp;
        const int32_t dir[3] = {0x1000, 0, 0};
        int32_t p[3] = {1000, 2000, 3000};
        check(s.addPoint(1, p, dir, 1), "seed point accepted");
        p[2] = 1000000;   // dz far above the 0x7B34 ceiling
        s.addPoint(1, p, dir, 1);
        check(!TA::markById(s, 1).active, "a point beyond the dz ceiling kills the strip");
    }

    // --- single-player uses 16 slots; a 17th distinct wheel finds no room ---
    {
        auto sp = std::make_unique<Skidmarks>();
        Skidmarks& s = *sp;
        const int32_t dir[3] = {0x1000, 0, 0};
        const int32_t p[3] = {1000, 2000, 3000};
        for (uint32_t id = 1; id <= 16; ++id) check(s.addPoint(id, p, dir, 1), "each of 16 wheels gets a slot");
        check(TA::usedSlots(s) == 16, "exactly 16 slots in use");
        check(!s.addPoint(17, p, dir, 1), "17th distinct wheel is refused in single player");
    }

    // --- lifetime: hold frames, then the mark deactivates, fades and is freed ---
    {
        auto sp = std::make_unique<Skidmarks>();
        Skidmarks& s = *sp;
        const int32_t dir[3] = {0x1000, 0, 0};
        const int32_t p[3] = {1000, 2000, 3000};
        s.addPoint(1, p, dir, 1);                 // hold = 1
        check(TA::markById(s, 1).active, "mark is live after a point");
        s.process();                              // hold 1 -> 0, still active
        check(TA::markById(s, 1).active, "still live while hold remains");
        s.process();                              // hold 0 -> deactivate
        check(!TA::markById(s, 1).active && TA::markById(s, 1).id, "deactivates once hold runs out, not yet freed");
        for (int i = 0; i < 200; ++i) s.process();
        check(TA::points(s) == 0 && TA::usedSlots(s) == 0, "mark fades through wait+alpha and frees its slot");
    }

    if (!failures) std::printf("skidmarks_test: all checks passed\n");
    return failures ? 1 : 0;
}
