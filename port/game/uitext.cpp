// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// InitStatsText (0x006c178c) builds the filename from the template "E_%s.gxt" (format literal @0x4728cb),
// overwriting the language byte, with %s = "stats", then opens it through cFileManager and parses the
// "DS_GXT" table. The English file is e_stats.gxt (present in port/data). StatsText (0x006c2544) returns the
// loaded table; callers index it by (id - GetNumberOfGlobalStrings). We reproduce that here on top of the
// port's existing GxtFile reader, loading from the user's own game data at run time.
#include "uitext.h"
#include "text/gxt.h"

bool UiText::load(const std::string& dataDir) {
    stats_.clear();
    GxtFile gxt;
    // English StatsText table. Non-English builds would load "<lang>_stats.gxt"; the port currently loads
    // e_main.gxt unconditionally (game.cpp), so this matches the active language.
    if (!gxt.load(dataDir + "/e_stats.gxt")) return false;
    stats_.reserve(gxt.count());
    for (size_t i = 0; i < gxt.count(); ++i) stats_.push_back(gxt.get(i));
    return !stats_.empty();
}

std::u16string UiText::statsText(uint16_t id) const {
    if (id < kNumGlobalStrings) return {};               // a GlobalText id: resolved from e_main.gxt instead
    const size_t i = (size_t)(id - kNumGlobalStrings);   // StatsText(id) indexing, matching the binary
    if (i >= stats_.size()) return {};
    std::u16string s = stats_[i];
    while (!s.empty() && s.back() == 0) s.pop_back();     // same trailing-NUL trim as Game::mainText / Mission::text
    return s;
}
