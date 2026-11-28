// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// cRoadBlocks::AddRoadBlock lane geometry (Traffic::roadBlockUnit): the formation of police cars across a road link
// and the two cops behind each car, checked against the native fixed-point formulas (0x8f68a0..0x8f69ac):
//   right = (dir.y, -dir.x); lane step = right*8 (the normalised Q12 direction << 3); the cars are centred on the
//   create position; each car's cops sit 2*dir behind it, flanking by +-right.
#include "traffic.h"
#include <cstdio>

static int failures;
static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
static bool eq(const int32_t a[3], int32_t x, int32_t y, int32_t z) { return a[0] == x && a[1] == y && a[2] == z; }

int main() {
    const int32_t Q = 4096;   // 1.0 in Q12

    // dir = +Y (unit), two lanes centred on the origin. right = (+X); step = 8 units in X.
    {
        const int32_t centre[3] = {0, 0, 0};
        const int32_t dir[3] = {0, Q, 0};
        const auto u0 = Traffic::roadBlockUnit(centre, dir, 2, 0);
        const auto u1 = Traffic::roadBlockUnit(centre, dir, 2, 1);
        check(eq(u0.car, -16384, 0, 0), "lane 0 car sits 4 units to the right of centre (half the 8-unit step)");
        check(eq(u1.car, 16384, 0, 0), "lane 1 car is one 8-unit step (right) from lane 0");
        check(eq(u0.copA, -12288, -8192, 0), "lane 0 cop A: 2 units behind (-2*dir), +right");
        check(eq(u0.copB, -20480, -8192, 0), "lane 0 cop B: 2 units behind (-2*dir), -right");
    }

    // dir = +X (unit), two lanes. right = (0,-Y); the step runs along -Y; the cops sit 2 units back along -X.
    {
        const int32_t centre[3] = {0, 0, 0};
        const int32_t dir[3] = {Q, 0, 0};
        const auto u0 = Traffic::roadBlockUnit(centre, dir, 2, 0);
        const auto u1 = Traffic::roadBlockUnit(centre, dir, 2, 1);
        check(eq(u0.car, 0, 16384, 0), "lane 0 car offset is perpendicular to the road direction");
        check(eq(u1.car, 0, -16384, 0), "the lane step is right*8 with right = (dir.y, -dir.x)");
        check(eq(u0.copA, -8192, 12288, 0), "cop A is 2*dir behind the car and +right");
        check(eq(u0.copB, -8192, 20480, 0), "cop B is 2*dir behind the car and -right");
    }

    // A single lane places the car exactly on the create position (no centring offset) and keeps its z.
    {
        const int32_t centre[3] = {5 * Q, 7 * Q, 2 * Q};
        const int32_t dir[3] = {0, Q, 0};
        const auto u = Traffic::roadBlockUnit(centre, dir, 1, 0);
        check(eq(u.car, 5 * Q, 7 * Q, 2 * Q), "a one-lane road puts the car on the create position, z preserved");
        check(u.copA[2] == 2 * Q && u.copB[2] == 2 * Q, "the cops keep the car's ground height");
    }

    if (failures) { std::fprintf(stderr, "%d road-block geometry check(s) failed\n", failures); return 1; }
    std::printf("road-block geometry OK\n");
    return 0;
}
