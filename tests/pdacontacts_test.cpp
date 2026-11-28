// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
// Tests for ContactsPdaApp (a port of Gui::cContactsApp): the encountered-mask build order, the name-id -> sprite
// index switches, and address-line resolution. Synthetic data only; no game assets. Does not call onRender (which
// needs the HUD backend); the row logic is exercised directly.
#include "../port/game/pdacontacts.h"
#include <cassert>
#include <cstdio>
#include <string>

using namespace pdacontacts;

static std::string narrow(const std::u16string& s) {
    std::string o;
    for (char16_t c : s) o += (c < 128) ? (char)c : '?';
    return o;
}

int main() {
    // --- small/large sprite switches (GetSpriteInfoForContact / GetLargeSpriteInfoForContact) ---
    assert(ContactSmallSpriteIndex(0x3a2) == 0xa);
    assert(ContactSmallSpriteIndex(0x3a3) == 0xa);
    assert(ContactSmallSpriteIndex(0x3a4) == 8);
    assert(ContactSmallSpriteIndex(0x3a6) == 0x14);
    assert(ContactSmallSpriteIndex(0x3ed) == 0x12);
    assert(ContactSmallSpriteIndex(0x111) == -1);      // outside the set
    assert(ContactLargeSpriteIndex(0x3a5) == 0);
    assert(ContactLargeSpriteIndex(0x3ac) == 7);
    assert(ContactLargeSpriteIndex(0x3ed) == 0x11);
    assert(ContactLargeSpriteIndex(0x999) == -1);

    // --- EncounteredContacts bit order (Init AddContact loop) ---
    assert(EncounteredContacts(0).empty());
    {
        auto c = EncounteredContacts(0x1);              // bit 0 -> contact 0
        assert(c.size() == 1 && c[0] == 0);
    }
    {
        auto c = EncounteredContacts(0b1111);           // bits 0..3 -> 0,1,2,3 in order
        assert(c.size() == 4 && c[0] == 0 && c[3] == 3);
    }
    {
        // bit 8 alternate-unlocks contact 2 when bit 2 is clear...
        auto c = EncounteredContacts(1u << 8);
        assert(c.size() == 1 && c[0] == 2);
    }
    {
        // ...but not twice when bit 2 is already set.
        auto c = EncounteredContacts((1u << 2) | (1u << 8));
        assert(c.size() == 1 && c[0] == 2);
    }
    {
        auto c = EncounteredContacts(1u << 9);          // bit 9 -> contact 9
        assert(c.size() == 1 && c[0] == 9);
    }

    // --- App: build rows with a name-id mapping + text provider ---
    auto nameId = [](int id) -> uint16_t {
        // Synthetic: map a couple of contacts onto known name ids so sprites resolve.
        if (id == 0) return 0x3a2;
        if (id == 1) return 0x3a5;
        return 0x3a7;
    };
    auto addrKey = [](int id) -> uint16_t { return (uint16_t)(0x52f + 0x30 + id); };
    auto text = [](uint16_t gxt) -> std::u16string {
        std::string s = "T" + std::to_string(gxt);
        return std::u16string(s.begin(), s.end());
    };

    ContactsPdaApp app;
    assert(!app.onLoad());                              // not ready before init()
    app.init(0b111, text, nameId, addrKey);             // contacts 0,1,2
    assert(app.onLoad());
    assert(app.headingId() == 0x3e);
    const auto& rows = app.rows();
    assert(rows.size() == 3);
    assert(rows[0].contactId == 0 && rows[0].nameTextId == 0x3a2 && rows[0].smallSprite == 0xa);
    assert(rows[0].available);
    assert(rows[1].smallSprite == 1 && rows[1].largeSprite == 0);

    // address line resolves AppText (key - 0x52f) through the provider
    assert(narrow(app.addressLine(rows[0])) == "T48");  // key = 0x52f+0x30+0 -> appTextId 0x30 = 48

    // --- selection via onInput hit-test (ContactList_OnClick) ---
    assert(app.selected() == -1);
    pda::Touch t; t.pressed = true;
    t.x = ContactsPdaApp::kListX + 10.f;
    t.y = ContactsPdaApp::kListY + ContactsPdaApp::kRowHeight * 1.5f;   // row index 1
    app.onInput(t);
    assert(app.selected() == rows[1].contactId);
    app.closePortrait();
    assert(app.selected() == -1);

    // --- no name mapping -> placeholder rows (no invented contacts) ---
    ContactsPdaApp app2;
    app2.init(0x1, text);                               // no nameId/addrKey
    assert(app2.rows().size() == 1);
    assert(!app2.rows()[0].available);
    assert(app2.rows()[0].addressKey == kDefaultAddressKey);

    std::printf("pdacontacts_test: all assertions passed\n");
    return 0;
}
