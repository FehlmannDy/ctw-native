// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
#include "bins.h"
#include <cstdlib>

// 12.12 fixed-point multiply, matching the house helper used across port/game (e.g. rigidbody.cpp).
// The binary lowers the decay as smull + lsr #12; for the non-negative magnitudes a settling bag
// carries this matches (a*b)>>12. Sign note: the original's lsr differs from an arithmetic shift for
// negative components, but the settle test below only inspects magnitudes via abs().
static inline int32_t mulq(int64_t a, int64_t b) { return (int32_t)((a * b) >> 12); }

// cBinBag::UpdateDynamic settle fragment (0x0085e940..0x0085e9b4): decay, then the three-axis test.
bool BinBag::settleStep(int32_t vel[3]) {
    for (int k = 0; k < 3; ++k) vel[k] = mulq(vel[k], kContactDecay);
    return std::abs(vel[0]) <= kSettleXY && std::abs(vel[1]) <= kSettleXY && std::abs(vel[2]) < kSettleZ;
}
