// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Gui::cContactsApp, the PDA "Contacts" application. See pdacontacts.h for the verified routine list.
//
// The two sprite-index routines (GetSpriteInfoForContact 0x007597e4, GetLargeSpriteInfoForContact 0x00759894) are a
// pure switch on the contact's name GXT id (immediates in the code); they are ported field-for-field below. The row
// build order and the per-row layout follow AddContact (0x007590cc). NO game text is baked: the contact-name
// GlobalText id comes from a caller-supplied table, the address line is resolved through the text provider, and
// sprites are referenced by sheet index only.
#include "pdacontacts.h"
#include "hud.h"
#include "gfx/spriteset.h"
#include "os/datafile.h"
#include "os/gamefs.h"
#include <glad/gl.h>
#include <stb_image.h>
#include <vector>

namespace pdacontacts {

namespace {
// Draw one SS_Contacts sprite by its index, reproducing GameplayHud::sprite / pda.cpp's drawAlwaysLoadedSprite: the
// loose ss_contacts.png is 2x the sheet's layout space (SpriteSet::pngScaleFor, "SS_" -> 2), so texels are the
// stored coordinate doubled; the quad spans the sprite's own size about its stored offX/offY handle, in the HUD's
// top-left pixel space. No art is embedded or recreated here -- only the user's own loaded sprite, blitted by id.
void drawContactsSprite(const SpriteSet& set, unsigned texture, int texW, int texH, int index,
                        float x, float y, float scale, float alpha, uint32_t tint) {
    if (texture == 0 || texW <= 0 || texH <= 0) return;
    if (index < 0 || index >= (int)set.sprites().size()) return;
    const SpriteDef& d = set.sprites()[index];
    const float w = d.w + 1.f, h = d.h + 1.f;   // cSprite::CalculateVerts includes the last stored pixel
    if ((d.x + w) * 2 > texW || (d.y + h) * 2 > texH) return;
    const float vx[4] = {-(float)d.offX, w - d.offX, w - d.offX, -(float)d.offX};
    const float vy[4] = {-(float)d.offY, -(float)d.offY, h - d.offY, h - d.offY};
    const float u[4] = {d.x * 2.f, (d.x + w) * 2.f, (d.x + w) * 2.f, d.x * 2.f};
    const float v[4] = {d.y * 2.f, d.y * 2.f, (d.y + h) * 2.f, (d.y + h) * 2.f};
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    glColor4f((tint >> 24) / 255.f, (tint >> 16 & 0xFF) / 255.f, (tint >> 8 & 0xFF) / 255.f,
              (tint & 0xFF) / 255.f * alpha);
    glBegin(GL_QUADS);
    for (int i = 0; i < 4; ++i) {
        glTexCoord2f(u[i] / texW, v[i] / texH);
        glVertex2f(x + vx[i] * scale, y + vy[i] * scale);
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}
}  // namespace


// ---------------------------------------------------------------------------------------------------------------
// GetSpriteInfoForContact (0x007597e4): the name GXT id -> small avatar sprite index switch. The id ranges and the
// resulting indices are taken directly from the compare/branch immediates. Ids outside the set leave the output
// unchanged in the binary (the caller keeps 0); here that is reported as -1.
// ---------------------------------------------------------------------------------------------------------------
int ContactSmallSpriteIndex(uint16_t id) {
    switch (id) {
        case 0x3a2: case 0x3a3: return 0xa;
        case 0x3a4:             return 8;
        case 0x3a5:             return 1;
        case 0x3a6:             return 0x14;
        case 0x3a7:             return 5;
        case 0x3a8: case 0x3a9: return 0x10;
        case 0x3aa: case 0x3ab: return 0xc;
        case 0x3ac:             return 8;
        case 0x3ed:             return 0x12;
        default:                return -1;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// GetLargeSpriteInfoForContact (0x00759894): the same id partition, different (portrait) sprite indices.
// ---------------------------------------------------------------------------------------------------------------
int ContactLargeSpriteIndex(uint16_t id) {
    switch (id) {
        case 0x3a2: case 0x3a3: return 9;
        case 0x3a4:             return 7;
        case 0x3a5:             return 0;
        case 0x3a6:             return 0x13;
        case 0x3a7:             return 4;
        case 0x3a8: case 0x3a9: return 0xf;
        case 0x3aa: case 0x3ab: return 0xb;
        case 0x3ac:             return 7;
        case 0x3ed:             return 0x11;
        default:                return -1;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// EncounteredContacts: cContactsApp::Init's AddContact loop (0x00758cbc). Bits 0..7 add contact ids 0..7 in order.
// Bit 8 is the special case at 0x758f24: it adds contact id 2, but only if bit 2 did not already add it. Bit 9 adds
// contact id 9. (The Init block tests bits 0..3, then 4..8, then 9; the order below reproduces that sequence.)
// ---------------------------------------------------------------------------------------------------------------
std::vector<int> EncounteredContacts(uint16_t mask) {
    std::vector<int> out;
    bool added2 = false;
    for (int bit = 0; bit <= 7; ++bit) {
        if (mask & (1u << bit)) {
            out.push_back(bit);
            if (bit == 2) added2 = true;
        }
    }
    if (mask & (1u << 8)) {           // 0x758f24: alternate unlock for contact 2
        if (!added2) { out.push_back(2); added2 = true; }
    }
    if (mask & (1u << 9)) out.push_back(9);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// ContactsPdaApp
// ---------------------------------------------------------------------------------------------------------------
void ContactsPdaApp::init(uint16_t encounteredMask, TextProvider text,
                          NameIdFn nameId, AddressKeyFn addressKey) {
    mask_ = encounteredMask;
    text_ = std::move(text);
    nameId_ = std::move(nameId);
    addressKey_ = std::move(addressKey);
    bound_ = true;
    rebuild();
}

void ContactsPdaApp::setEncounteredMask(uint16_t mask) {
    mask_ = mask;
    rebuild();
}

void ContactsPdaApp::rebuild() {
    rows_.clear();
    selected_ = -1;
    for (int id : EncounteredContacts(mask_)) {
        ContactRow r;
        r.contactId = id;
        // Name GXT id drives both the displayed name and the avatar sprite; it lives in a .data table the port does
        // not bake. Without the caller's mapping the row is a documented placeholder (no invented contact).
        if (nameId_) {
            r.nameTextId = nameId_(id);
            r.available = true;
        } else {
            r.nameTextId = 0;
            r.available = false;
        }
        r.smallSprite = ContactSmallSpriteIndex(r.nameTextId);
        r.largeSprite = ContactLargeSpriteIndex(r.nameTextId);
        // Gui::GetContactAddressKey (0x007aa774): ids 1..8 look up position-based keys (save + cContactManager, not
        // tracked by the port); anything else is 0x55a. The caller supplies the lookup when available.
        r.addressKey = addressKey_ ? addressKey_(id) : kDefaultAddressKey;
        rows_.push_back(r);
    }
}

void ContactsPdaApp::select(int contactId) {
    for (const ContactRow& r : rows_) {
        if (r.contactId == contactId) { selected_ = contactId; return; }
    }
}

std::u16string ContactsPdaApp::addressLine(const ContactRow& r) const {
    // AddContact (0x007590cc): AppText::GetString(key - 0x52f) supplies the address text, then AppText format 0x2c
    // wraps it. The port resolves the underlying AppText id through the text provider; the format wrapping (0x2c)
    // is left to the provider, which owns the GXT substitution. No text is baked here.
    // key - 0x52f is the AppText id of the raw address string (AddContact 0x759278).
    uint16_t appTextId = (uint16_t)(r.addressKey - 0x52f);
    if (!text_) {
        std::string k = "#" + std::to_string(appTextId);   // neutral key, like PdaStats_FormatValue
        return std::u16string(k.begin(), k.end());
    }
    return text_(appTextId);
}

ContactsPdaApp::~ContactsPdaApp() {
    if (sheetTexture_) glDeleteTextures(1, &sheetTexture_);
    sheetTexture_ = 0;
}

// Load SS_Contacts the same way Pda::loadTheme loads SS_PdaSprites and GameplayHud::init loads SS_AlwaysLoaded: the
// definitions from "SS_Contacts.bin" through GameFs (ROM.WAD), parsed into a SpriteSet; the loose "ss_contacts.png"
// read and uploaded as an RGBA GL texture with the same LINEAR/CLAMP state the HUD uses; sheet + texture + size are
// owned by the app. SS_Contacts is cContactsApp's avatar set (cContactsApp::Init 0x00758d94, name at 0x00476287),
// and the binary uses it for BOTH the small list avatar and the large portrait (app+0x130). No asset is committed;
// this only reads the user's own data files by name and keeps them as a texture to blit by id.
bool ContactsPdaApp::loadSheet(const std::string& dataDir) {
    // Drop any previously loaded texture so a re-load does not leak.
    if (sheetTexture_) glDeleteTextures(1, &sheetTexture_);
    sheetTexture_ = 0;
    sheetTexW_ = sheetTexH_ = 0;
    sheet_ = SpriteSet{};

    GameFs fs;
    std::vector<uint8_t> defs, png;
    if (!fs.open(dataDir) || !fs.read("SS_Contacts.bin", defs) || !sheet_.parse(defs) ||
        !Data_ReadAll(dataDir + "/ss_contacts.png", png)) {
        sheet_ = SpriteSet{};
        return false;
    }
    int channels;
    unsigned char* pixels = stbi_load_from_memory(png.data(), (int)png.size(), &sheetTexW_, &sheetTexH_, &channels, 4);
    if (!pixels) {
        sheet_ = SpriteSet{};
        sheetTexW_ = sheetTexH_ = 0;
        return false;
    }
    glGenTextures(1, &sheetTexture_);
    glBindTexture(GL_TEXTURE_2D, sheetTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sheetTexW_, sheetTexH_, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);
    return true;
}

// cContactsApp::ContactList_OnClick (0x00759750): the shell routes a touch here; the original hit-tests the list box.
// With only a Touch (window pixels) we reproduce the row hit-test on the same grid the renderer uses.
void ContactsPdaApp::onInput(const pda::Touch& t) {
    if (!t.pressed) return;
    if (t.x < kListX || t.x > kListX + kListWidth) return;
    int idx = (int)((t.y - kListY) / kRowHeight);
    if (idx < 0 || idx >= (int)rows_.size()) return;
    select(rows_[idx].contactId);    // SetupContactSprites: show that contact's portrait
}

// cContactsApp::Render (0x00759d38): the main pass draws the list rows (RenderLines: scale 1, 0x19E5E5E5, font 4);
// the overlay pass draws the selected contact's large portrait. We draw directly with the port's HUD primitives.
void ContactsPdaApp::onRender(int /*w*/, int /*h*/) const {
    auto u16 = [](const std::string& s) { return std::u16string(s.begin(), s.end()); };
    const bool haveSheet = sheetLoaded();
    for (int i = 0; i < (int)rows_.size(); ++i) {
        const ContactRow& r = rows_[i];
        const float y = kListY + i * kRowHeight;
        // The avatar: cContactsApp::AddContact (0x00759224) adds the row's SS_Contacts sprite by its small index
        // (ContactSmallSpriteIndex; AddContact keeps index 0 when the name id is outside the set). We blit the user's
        // own loaded sprite, fit into the kAvatarSize (0x14) box AddContact reserves and centred in the row. With no
        // sheet loaded, the previous slot rect stands in (no-op on the art).
        if (haveSheet) {
            const int idx = r.smallSprite >= 0 ? r.smallSprite : 0;   // AddContact's fallback to 0
            if (idx >= 0 && idx < (int)sheet_.sprites().size()) {
                const SpriteDef& d = sheet_.sprites()[idx];
                const float sw = d.w + 1.f, sh = d.h + 1.f;
                const float s = sw > 0.f ? kAvatarSize / sw : 1.f;    // fit the native sprite into the 20px slot
                const float px = kListX + d.offX * s;                 // put the sprite bbox top-left at the slot
                const float py = y + (kRowHeight - sh * s) * 0.5f + d.offY * s;  // centre it vertically in the row
                drawContactsSprite(sheet_, sheetTexture_, sheetTexW_, sheetTexH_, idx, px, py, s, 1.f, 0xFFFFFFFFu);
            }
        } else {
            // Avatar slot (sprite index referenced only; stands in for the art when the sheet is not loaded).
            Hud_Rect(kListX, y, kAvatarSize, kAvatarSize, kRowColor);
        }
        const float textX = kListX + kAvatarSize + 6.f;
        std::u16string name = r.available && text_ ? text_(r.nameTextId)
                                                   : (u"#" + u16(std::to_string(r.nameTextId)));
        Hud_Text16(textX, y, kFontScale, kRowColor, name);
        std::u16string addr = addressLine(r);
        if (!addr.empty())
            Hud_Text16(textX, y + kRowHeight * 0.5f, kFontScale, kRowColor, addr);
    }
    // Selected contact: the large portrait (cContactsApp::SetupContactSprites 0x00759bd8/0x00759bfc adds it from the
    // same SS_Contacts sheet, index = ContactLargeSpriteIndex, at window (0x4b,0x57) and native scale 1.0). We blit
    // the user's own loaded portrait sprite by that id; with no sheet loaded, the previous marker region stands in.
    if (selected_ >= 0) {
        const ContactRow* sel = nullptr;
        for (const ContactRow& r : rows_)
            if (r.contactId == selected_) { sel = &r; break; }
        if (haveSheet && sel) {
            const int idx = sel->largeSprite >= 0 ? sel->largeSprite : 0;  // SetupContactSprites' fallback to 0
            drawContactsSprite(sheet_, sheetTexture_, sheetTexW_, sheetTexH_, idx,
                               kPortraitAvatarX, kPortraitAvatarY, kPortraitAvatarScale, 1.f, 0xFFFFFFFFu);
        } else if (!haveSheet) {
            Hud_Rect(kPortraitX, kPortraitY, 150.f, 150.f, kRowColor);
        }
    }
}

}  // namespace pdacontacts
