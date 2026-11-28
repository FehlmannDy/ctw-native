// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// UiText: resolves the StatsText GXT ids the PDA's Stats app uses (cPauseStatsApp labels/titles,
// roughly 0x530..0x5a2 and 0x600+). These are NOT GlobalText ids: e_main.gxt only holds the
// GetNumberOfGlobalStrings (@0x006c23a4 -> 0x52E) global strings for ids 0..0x52D. StatsText lives in a
// separate table that InitStatsText (@0x006c178c) loads from "<lang>_stats.gxt" (the engine's filename
// template is "E_%s.gxt" with %s = "stats"; English -> e_stats.gxt in port/data). StatsText(id) indexes
// that table by (id - GetNumberOfGlobalStrings), exactly as Mission::text / the mission e_<name>.gxt tables
// index by (id - global count). No game text is baked here: strings load at run time from the user's own
// e_stats.gxt via the port's GxtFile reader. Reference by file name / id only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

class UiText {
public:
    // GetNumberOfGlobalStrings (@0x006c23a4): the number of e_main.gxt GlobalText strings. StatsText ids are
    // biased by this count, i.e. the stats table index is (id - kNumGlobalStrings).
    static constexpr uint16_t kNumGlobalStrings = 0x52E;

    // Load the StatsText table (e_stats.gxt) once, after the data dir is known. Returns false (and leaves the
    // resolver empty) when the file is absent or malformed, so the caller still falls back to "#id".
    bool load(const std::string& dataDir);

    bool loaded() const { return !stats_.empty(); }

    // Resolve a StatsText id (>= kNumGlobalStrings). Returns the string with trailing NULs stripped, or an
    // empty string for an unknown / out-of-range id so the caller can fall back to the "#id" placeholder.
    std::u16string statsText(uint16_t id) const;

private:
    std::vector<std::u16string> stats_;   // e_stats.gxt, indexed by (id - kNumGlobalStrings)
};
