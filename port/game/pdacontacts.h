// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// ContactsPdaApp: a port of Gui::cContactsApp, the PDA "Contacts" application (libGame.so
// 0x00758b90..0x00759d38). It builds the list of contacts the player has encountered, one row per contact
// (a small avatar sprite + the contact's name + a formatted address/number line), and shows a large portrait
// for the selected contact. This file ports the *list-build logic*, the *name-id -> avatar-sprite mapping*
// and the *layout*; it bakes NO game text: names and address lines are GXT string ids resolved at run time
// through a caller-supplied text provider, exactly as PdaStatsApp does. Sprites are referenced by index only.
//
// Verified routines (address | evidence; observation unless marked assumption):
//   cContactsApp::Init                       0x00758cbc  cPdaApp::Init, cListBox::Init(+0x190), AddHeading with
//                                                         GlobalText id 0x3e; reads SaveGameImpl::ContactsEncountered()
//                                                         (a u16 mask) and calls AddContact for each set bit 0..9.
//   cContactsApp::AddContact                 0x007590cc  one row: name GXT id = nameTable[id] (GlobalText), cListBox::
//                                                         AddItem (stores contact id at item+0xf8), a small avatar
//                                                         sprite (index via the name-id switch), and an AppText
//                                                         address line (GetContactAddressKey -> AppText[key-0x52f],
//                                                         wrapped by AppText format 0x2c).
//   cContactsApp::GetSpriteInfoForContact    0x007597e4  name GXT id -> small avatar sprite index (the switch below).
//   cContactsApp::GetLargeSpriteInfoForContact 0x00759894 name GXT id -> large portrait sprite index.
//   cContactsApp::Render                     0x00759d38  main pass: cListBox::RenderLines(scale 1, 0x19E5E5E5, font 4);
//                                                         overlay pass draws the selected-portrait fade quad.
//   cContactsApp::Process                    0x007594c8  SetPhoneTextScale(1.5), cPdaApp::Process, step transitions.
//   cContactsApp::ContactList_OnClick        0x00759750  tap a row -> read item+0xf8 contact id, SetupContactSprites.
//   cContactsApp::OnSwipeLeft                0x00759cac  swipe/back: close the open portrait, else pop the app stack.
//   SaveGameImpl::ContactsEncountered        0x008bc5c0  the contact source: a u16 bitmask at script section +0x704.
//   Gui::GetContactAddressKey                0x007aa774  address GXT key for a contact (id 1..8), else 0x55a.
//
// Deferred (asset / cross-system, out of this file's scope, reported): the real sprite-sheet name and the sprite
// ids behind the avatar indices (cSpriteDataSet at 0x476287, Gui::GetContactInfo), the contact-name GlobalText id
// table (.data 0xdcf000+0x8c0) and the address-key tables + cContactManager::GetCurPosId (0x481528 / 0x4815e8) --
// all supplied to init() by the caller; the help-tip manager, audio sfx, and cListBox pixel metrics.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "pda.h"
#include "gfx/spriteset.h"   // owned SS_Contacts avatar sheet loaded by loadSheet()

namespace pdacontacts {

// eContactID: the contacts Init iterates (bits 0..9 of ContactsEncountered). The numeric ids are the bit indices;
// the names/sprites are keyed off each contact's GlobalText name id, not off this id directly.
enum : int { kNumContacts = 10 };

// The app heading GXT id (cContactsApp::Init: GlobalText 0x3e). A GXT string id, resolved by the text provider.
constexpr uint16_t kHeadingTextId = 0x3e;

// AppText format id wrapping the address line (AddContact: AppText 0x2c). Resolved by the text provider.
constexpr uint16_t kAddressFormatTextId = 0x2c;

// Default address key when a contact has no position-based address (GetContactAddressKey: ids outside 1..8).
constexpr uint16_t kDefaultAddressKey = 0x55a;

// Small avatar sprite index for a contact's name GXT id (cContactsApp::GetSpriteInfoForContact 0x007597e4).
// Returns -1 when the name id is outside the known set (AddContact then falls back to sprite index 0).
int ContactSmallSpriteIndex(uint16_t nameTextId);

// Large portrait sprite index for a contact's name GXT id (cContactsApp::GetLargeSpriteInfoForContact 0x00759894).
// Returns -1 when the name id is outside the known set.
int ContactLargeSpriteIndex(uint16_t nameTextId);

// The order Init adds contacts from a ContactsEncountered() bitmask (cContactsApp::Init 0x00758cbc). Bits 0..9 map
// to contact ids 0..9 in order; bit 8 is the documented special case -- it adds contact id 2 only if bit 2 did not
// already add it. Returns the contact ids in display order.
std::vector<int> EncounteredContacts(uint16_t encounteredMask);

// One built contact row, ready to draw. `nameTextId` is a GlobalText GXT id (from the caller-supplied name table);
// `addressKey` is the Gui::GetContactAddressKey result; `smallSprite`/`largeSprite` are sprite-sheet indices (-1 =
// none). `available` is false for a row whose name id the port could not resolve (placeholder, layout preserved).
struct ContactRow {
    int contactId = 0;
    uint16_t nameTextId = 0;
    uint16_t addressKey = kDefaultAddressKey;
    int smallSprite = -1;
    int largeSprite = -1;
    bool available = true;
};

// The standalone component the PDA shell hosts, overriding the cPdaApp hooks. The orchestrator registers this in
// the PDA app factory. init() binds the contact source (the ContactsEncountered() u16) and the run-time resolvers:
//   text        - GXT provider (GlobalText/AppText id -> text), like PdaStatsApp's.
//   nameId      - contact id -> its GlobalText name id (the .data table this file does not bake). When null, rows
//                 are marked unavailable and a placeholder key is shown (no invented contacts).
//   addressKey  - contact id -> Gui::GetContactAddressKey result (needs save + cContactManager). When null, every
//                 row uses kDefaultAddressKey (0x55a), matching an out-of-range contact.
class ContactsPdaApp : public pda::PdaApp {
public:
    using TextProvider = std::function<std::u16string(uint16_t)>;
    using NameIdFn     = std::function<uint16_t(int contactId)>;
    using AddressKeyFn = std::function<uint16_t(int contactId)>;

    // A placeholder ePdaApp id; the orchestrator supplies the real value when it registers the app.
    static constexpr int kAppContactsPlaceholder = 0x4b;   // assumption: not confirmed against the binary's enum

    // The ePdaApp id is owned by the orchestrator's enum (see pda.h). The caller passes the real id; the default is
    // a documented placeholder so the app is usable standalone and in tests.
    explicit ContactsPdaApp(int appId = kAppContactsPlaceholder) : pda::PdaApp(appId) {}

    // Owns a GL texture once loadSheet() succeeds; not copyable (would double-free it). Held by the PDA factory as
    // a stable unique_ptr<PdaApp>, so a virtual destructor releasing the texture is safe.
    ~ContactsPdaApp() override;
    ContactsPdaApp(const ContactsPdaApp&) = delete;
    ContactsPdaApp& operator=(const ContactsPdaApp&) = delete;

    void init(uint16_t encounteredMask, TextProvider text,
              NameIdFn nameId = {}, AddressKeyFn addressKey = {});

    // Load the contacts avatar sprite sheet the same way GameplayHud::init loads SS_AlwaysLoaded and Pda::loadTheme
    // loads SS_PdaSprites: the definitions come from "SS_Contacts.bin" (the cSpriteDataSet name the binary passes in
    // cContactsApp::Init 0x00758d94 -> cSpriteDataSet::cSpriteDataSet, string at 0x00476287) read through GameFs
    // (ROM.WAD), and the pixels from the loose "ss_contacts.png" (2x the layout space, the SS_ PNG convention). The
    // parsed SpriteSet + GL texture are owned by this app; onRender draws each row's avatar by its already-computed
    // index once this succeeds. Returns false and leaves the app sheet-less (render keeps the slot look) when any
    // part is missing, so the orchestrator may call it once after the data dir is ready (like Pda::loadTheme).
    //
    // Observation (from the binary): cContactsApp uses THIS one sheet (app+0x130) for BOTH the small list avatar
    // (AddContact 0x00759224) and the large portrait (SetupContactSprites 0x00759bfc); ss_portraits is not used by
    // cContactsApp.
    bool loadSheet(const std::string& dataDir);
    bool sheetLoaded() const { return sheetTexture_ != 0 && !sheet_.sprites().empty(); }
    const SpriteSet& sheet() const { return sheet_; }
    unsigned sheetTexture() const { return sheetTexture_; }

    // Rebuild the row list from a (new) encountered mask (cContactsApp::Init's AddContact loop).
    void setEncounteredMask(uint16_t mask);
    uint16_t encounteredMask() const { return mask_; }

    const std::vector<ContactRow>& rows() const { return rows_; }
    uint16_t headingId() const { return kHeadingTextId; }

    // The currently selected contact (ContactList_OnClick stores item+0xf8 into +0x390); -1 when none is open.
    int selected() const { return selected_; }
    void select(int contactId);          // cContactsApp::ContactList_OnClick + SetupContactSprites
    void closePortrait() { selected_ = -1; }  // cContactsApp::OnSwipeLeft closing the open portrait

    // Resolve a row's address line (AddContact: AppText format 0x2c with AppText[key-0x52f] substituted). When no
    // text provider is bound, returns a neutral "#<key>" key, mirroring PdaStats_FormatValue.
    std::u16string addressLine(const ContactRow& r) const;

    // cPdaApp hooks.
    bool onLoad() override { return bound_; }   // ready once init() has bound the providers
    void onProcess() override {}                // Process (0x7594c8): no stateful per-frame work without live input
    void onInput(const pda::Touch& t) override; // tap selects a row; implemented via row hit-testing
    void onRender(int w, int h) const override; // Render (0x759d38): draw the rows, then the selected portrait

    // Layout in the PDA's 1024x768 content space (observed anchors from Init / AddContact; cListBox row metrics
    // are approximated -- the exact pitch lives in cListBox, which is deferred).
    static constexpr float kListX = 240.f;       // small-avatar window pos.x (Init AddSpriteWindow 0x168 -> list area)
    static constexpr float kListY = 68.f;        // first row y (AddItem y 0x2c + heading)
    static constexpr float kRowHeight = 44.f;    // AddItem row height arg (0x2c)
    static constexpr float kListWidth = 480.f;   // AddItem/AddTextSprite width (0x1e0)
    static constexpr float kAvatarSize = 20.f;   // AddItemSprite size (0x14)
    static constexpr float kPortraitX = 75.f;    // SetupContactSprites large window pos.x (0x4b)
    static constexpr float kPortraitY = 150.f;   // SetupContactSprites large window pos.y (0x96)
    // The large portrait AVATAR anchor and scale (cContactsApp::SetupContactSprites 0x00759bd8/0x00759bfc:
    // AddSpriteWindow at (0x4b,0x57) using the +0x130 sheet, index = ContactLargeSpriteIndex, drawn at cFixed 1.0).
    static constexpr float kPortraitAvatarX = 75.f;   // 0x4b
    static constexpr float kPortraitAvatarY = 87.f;   // 0x57
    static constexpr float kPortraitAvatarScale = 1.f;  // SetupContactSprites adds the portrait at native scale 1.0
    static constexpr float kFontScale = 1.f;
    static constexpr int kFont = 4;              // cListBox::RenderLines font argument
    static constexpr uint32_t kRowColor = 0x19E5E5E5u;  // cContactsApp::Render tint (RGBA)

private:
    void rebuild();

    uint16_t mask_ = 0;
    TextProvider text_;
    NameIdFn nameId_;
    AddressKeyFn addressKey_;
    std::vector<ContactRow> rows_;
    int selected_ = -1;
    bool bound_ = false;

    // SS_Contacts (the cSpriteDataSet at cContactsApp +0x130): the small list avatars AND the large portraits.
    SpriteSet sheet_;
    unsigned sheetTexture_ = 0;   // ss_contacts.png as a GL texture (owned; released in the destructor)
    int sheetTexW_ = 0, sheetTexH_ = 0;
};

}  // namespace pdacontacts
