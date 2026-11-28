// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
#pragma once
#include <cstdint>

// cBinBag - the street trash-bag dynamic prop (a cDynamicProp subclass, ClassId 0x24, in the
// 0x23..0x27 dynamic-prop range noted in propdynamics.cpp). libGame.so has only three routines:
//   cBinBag::ClassId        0x0085e834  (mov w0,#0x24; ret)
//   cBinBag::UpdateDynamic  0x0085e83c  (the per-frame update)
//   cBinBag::~cBinBag       0x0085e9bc  (base dtor + cPool::Free; inseparable from the shared object)
//
// The live per-frame update is inseparable from the shared cDynamicProp/cPhysical object (vtable
// dispatch, GetWorldCGPos, SetToPhysics) which is off-limits here; PropDynamics already drives the
// generic loose-prop settle (port/game/propdynamics.cpp). This module holds only the separable,
// pure fragments that are specific to the bin bag and verified against the binary.
struct BinBag {
    // cBinBag::ClassId  (0x0085e834).
    static constexpr int kClassId = 0x24;
    static constexpr int ClassId() { return kClassId; }

    // The bin-bag's own per-frame contact-settle velocity decay, applied inside UpdateDynamic while
    // the body reports contact (0x0085e940: tbz on bit 3 of the byte at +0x4EA). Each of the three
    // signed velocity components at +0xB8/+0xBC/+0xC0 is scaled by (v * 0x8CC) >> 12 and written back
    // (0x0085e94c mov w9,#0x8CC; smull; lsr #12; stp/str). 0x8CC/0x1000 ~= 0.5498.
    // OBSERVATION: this bin-specific factor is stronger than cPhysical's generic damp 0xF3C
    // (rigidbody.cpp:142, ~0.9521) that PropDynamics currently applies to every loose prop.
    static constexpr int32_t kContactDecay = 0x8CC;

    // Settle thresholds compared against the decayed velocity (0x0085e98c..0x0085e9a0): abs(vx) and
    // abs(vy) must be <= 0x3FF, abs(vz) must be < 0x2000; then SetToPhysics(false) is called
    // (0x0085e9a4). Identical constants to the live predicate in propdynamics.cpp:533.
    static constexpr int32_t kSettleXY = 0x3FF;
    static constexpr int32_t kSettleZ  = 0x2000;

    // One contact-settle step on a 12.12-fixed velocity triple: decays each component in place by
    // kContactDecay and returns true when the decayed velocity is small enough that UpdateDynamic
    // would take the body out of physics. Pure; no engine state.
    static bool settleStep(int32_t vel[3]);
};
