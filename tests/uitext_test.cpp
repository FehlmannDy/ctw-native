// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
// Tests for UiText (StatsText GXT resolver): the (id - GetNumberOfGlobalStrings) indexing and NUL trim.
// Synthetic data only; no game assets. Writes a tiny DS_GXT file to a temp path and reads it back.
#include "../port/game/uitext.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0777)
#endif

// Build a minimal DS_GXT blob: "DS_GXT", u16 count, then count * { u16 len, u16 chars[len] } (UTF-16LE).
static std::vector<uint8_t> makeGxt(const std::vector<std::u16string>& strings) {
    std::vector<uint8_t> b = {'D', 'S', '_', 'G', 'X', 'T'};
    auto put16 = [&](uint16_t v) { b.push_back((uint8_t)(v & 0xFF)); b.push_back((uint8_t)(v >> 8)); };
    put16((uint16_t)strings.size());
    for (const auto& s : strings) {
        put16((uint16_t)s.size());
        for (char16_t c : s) put16((uint16_t)c);
    }
    return b;
}

int main() {
    // A table whose index 2 == id 0x530 and index 3 == id 0x531 under the 0x52E bias.
    std::vector<std::u16string> t(4, u"");
    t[2] = u"General";          // id 0x52E + 2 = 0x530
    t[3] = std::u16string(u"X") + char16_t(0);   // trailing NUL must be trimmed
    auto blob = makeGxt(t);

    // UiText::load appends "/e_stats.gxt", so create a temp dir and write that exact name under it.
    std::string dir = std::string(std::tmpnam(nullptr)) + "_uitext";
    MKDIR(dir.c_str());   // ok if it already exists
    std::string gxtPath = dir + "/e_stats.gxt";
    FILE* f = std::fopen(gxtPath.c_str(), "wb");
    assert(f);
    std::fwrite(blob.data(), 1, blob.size(), f);
    std::fclose(f);

    UiText ui;
    assert(ui.load(dir));
    assert(ui.loaded());

    assert(ui.statsText(0x530) == std::u16string(u"General"));   // index 2
    assert(ui.statsText(0x531) == std::u16string(u"X"));         // index 3, NUL trimmed
    assert(ui.statsText(0x52E).empty());                          // index 0 (empty string in table)
    assert(ui.statsText(0x100).empty());                          // a GlobalText id -> not ours
    assert(ui.statsText(0x9999).empty());                         // out of range

    UiText missing;
    assert(!missing.load(std::string(std::tmpnam(nullptr))));     // absent file -> not loaded
    assert(missing.statsText(0x530).empty());                     // and empty for every id

    std::printf("uitext_test OK\n");
    return 0;
}
