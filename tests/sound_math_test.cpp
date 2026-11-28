// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
// Distance attenuation and stereo pan of cAudioManager::ProcessStandardSfx on synthetic inputs. No original audio
// data is embedded: the expected numbers are recomputed from the published fixed-point curves (ComputeVolume's
// 0x333 near-field split and square-law fall-off; ComputePan's 0x100000000000 reciprocal, 0x24/0x198 forward
// clamps and the 0x80 centre).
#include "sound.h"
#include <cstdint>
#include <cstdio>

static int failures;
static void check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; }
}

// SoundTestAccess is the friend declared in sound.h; it reaches the private, hardware-free math helpers.
struct SoundTestAccess {
    static int vol(int volume, int radius, uint32_t d2) { return Sound::computeVolume(volume, radius, d2); }
    static int pan(float lx, float lz) { return Sound::computePan(lx, lz); }
};

static void volumeTests() {
    using T = SoundTestAccess;
    const int V = 100, R = 1000;
    // near-field: the full volume within 0.2 x radius (0x333 >> 12 = 199 squared units here)
    check(T::vol(V, R, 0) == V, "a source on the listener plays at full volume");
    check(T::vol(V, R, 198) == V, "just inside the near-field radius is still full volume");
    check(T::vol(V, R, 199) == V, "the near-field boundary is full volume");
    // square-law fall-off beyond the near field, down to silence at the radius edge
    check(T::vol(V, R, 500) == 38, "the mid-range fall-off matches the square-law curve");
    check(T::vol(V, R, R) == 0, "a source at the exact radius edge is silent");
    int prev = V;
    for (uint32_t d = 199; d <= (uint32_t)R; ++d) {
        int v = T::vol(V, R, d);
        check(v >= 0 && v <= V, "the fall-off stays within [0, volume]");
        check(v <= prev, "the fall-off never rises with distance");
        prev = v;
    }
    // culls
    check(T::vol(V, R, R + 1) == 0, "beyond the radius there is no sound");
    check(T::vol(V, R, 100001) == 0, "beyond the 100000-unit cull there is no sound");
    check(T::vol(V, 0, 0) == 0, "a zero radius is silent");
    check(T::vol(V, -5, 0) == 0, "a negative radius is silent");
    // volume scales the curve linearly at a fixed distance
    check(T::vol(0, R, 500) == 0, "a zero-volume event is silent everywhere");
    check(T::vol(64, R, 500) == 24 && T::vol(127, R, 500) == 49,
          "the fall-off scales linearly with the event volume");
}

static void panTests() {
    using T = SoundTestAccess;
    // straight ahead is centred (0x40), whatever the forward distance
    check(T::pan(0.f, 1.f) == 0x40, "a source dead ahead and close pans centre");
    check(T::pan(0.f, 100.f) == 0x40, "a source dead ahead pans centre");
    check(T::pan(0.f, 500.f) == 0x3F, "far ahead saturates one step left of centre (the 0x198 clamp)");
    // sign: +lx (camera right) drives the pan toward 0, -lx toward 127, symmetric about centre
    check(T::pan(1.f, 100.f) == 56 && T::pan(-1.f, 100.f) == 71, "a small offset pans symmetrically about centre");
    check(T::pan(1.f, 100.f) + T::pan(-1.f, 100.f) == 0x7F, "the two sides are mirror images");
    check(T::pan(1.f, 100.f) < 0x40 && T::pan(-1.f, 100.f) > 0x40, "right is below centre, left above");
    // hard offsets clamp to the extremes
    check(T::pan(100.f, 100.f) == 0, "a hard-right source clamps to 0");
    check(T::pan(-100.f, 100.f) == 0x7F, "a hard-left source clamps to 127");
    // every output stays in range
    for (int i = -200; i <= 200; ++i) {
        int p = T::pan((float)i, 100.f);
        check(p >= 0 && p <= 0x7F, "the pan stays within [0, 127]");
    }
}

int main() {
    volumeTests();
    panTests();
    if (failures) { std::fprintf(stderr, "%d sound-math check(s) failed\n", failures); return 1; }
    std::printf("sound math OK\n");
    return 0;
}
