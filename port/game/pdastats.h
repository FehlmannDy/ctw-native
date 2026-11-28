// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// PdaStatsApp: a port of Gui::cPauseStatsApp (the pause menu / PDA "Stats" application, libGame.so
// 0x0083fbf0..0x00846e78). It renders the stats pages: a category of rows, each row a GXT label on the left and a
// formatted counter on the right. This file ports the *layout* and the *row order* (which counter is shown in which
// row, in which category) and maps each row to the port's Stats field. It deliberately bakes NO game text: labels are
// StatsText GXT string ids resolved at run time through a caller-supplied text provider.
//
// Verified routines (address | evidence):
//   cPauseStatsApp::Init              0x0083fbf0  app set-up, 3-5 widget-bar category buttons, first BuildStats
//   cPauseStatsApp::SetTitle          0x00840250  per-screen heading GXT id (StatsText 0x531..0x53d / GlobalText 0x3a3..)
//   cPauseStatsApp::BuildStats        0x00841778  jump table -> one Build<Category>Stats; builds the primary cListBox
//   cPauseStatsApp::BuildSubStats     0x00842644  the score/replay sub-screens
//   cPauseStatsApp::BuildGeneralStats 0x00842a4c  21 rows
//   cPauseStatsApp::BuildCrimeStats   0x00842e7c  7 rows
//   cPauseStatsApp::BuildVehicleStats 0x00843048  21 rows
//   cPauseStatsApp::BuildMoneyStats   0x00843458  12 rows
//   cPauseStatsApp::BuildCombatStats  0x00843968  7 rows
//   cPauseStatsApp::AddStatToListBox  0x00845e44  one data row (label + FormatValue + optional medal sprite)
//   cPauseStatsApp::AddHeaderToListBox0x00846058  a header / contact row
//   cPauseStatsApp::FormatValue       0x008462bc  the 16-way value formatter (jump table @0x485ea8)
//   cPauseStatsApp::Render            0x008469ac  draws the primary (+0x198) and sub (+0x1a8) list boxes
//
// sStat, the 28-byte row descriptor the Build routines fill and AddStatToListBox/FormatValue consume
// (offsets confirmed from the disassembly and the rodata constants):
//   +0x00 u32 labelId     StatsText GXT string id (stored biased by GetNumberOfGlobalStrings at run time)
//   +0x04 u32 (unused, 0)
//   +0x08 u32 value       the primary counter
//   +0x0c u32 value2      a second counter (only the time-range format uses it)
//   +0x10 u32 format      one of eStatFormat (FormatValue's jump-table index; also a layout hint in AddStatToListBox)
//   +0x14 u32 sprite      0 none, else a medal sprite kind (1->SS_AlwaysLoaded 0x52, 2->0x54, other->0x53)
//   +0x18 u32 (unused, 0)
#pragma once
#include "stats.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class SpriteSet;   // port/src/gfx/spriteset.h: the SS_AlwaysLoaded medal sheet, supplied by the shell

// FormatValue's number formats (jump table @0x485ea8, base 0x846314). The index is sStat.format.
enum class eStatFormat : uint8_t {
    Plain = 0,          // "%d"
    TwoDecimal = 1,     // value/100 "." value%100
    Distance = 2,       // 0x846404: scaled distance (metres/feet by IsAPhone)
    Thousands = 3,      // 0x8465f4: value / 1000
    FixedQ12 = 4,       // 0x846650: value / 0x1000 with two decimals (used by accuracy %)
    Money = 5,          // 0x846478: PrintFP "$"
    MinutesSeconds = 6, // 0x846558: value / 60
    HoursMinSec = 7,    // 0x846580: hours / minutes / seconds
    TimeElapsed = 8,    // 0x8467b4: cCounter::TimeElapsed(value)
    TimeElapsedRange = 9,  // 0x84668c: TimeElapsed(value) ".." TimeElapsed(value2)
    Ratio = 10,         // 0x846354: value + StatsText 0x52e (" / ") + value
    GlobalTextId = 11,  // 0x8464dc: value is a GlobalText string id
    CustomPercent = 12, // 0x8463ec
    StatsTextId = 13,   // 0x84632c: value is a StatsText string id (weapon / radio-station name)
    Identity = 14,      // 0x846820: no numeric formatting
    AppTextId = 15,     // 0x8465cc: value is an AppText string id
};

// The stat categories cPauseStatsApp drives. The names follow the Build<Category>Stats routines; the numeric
// eStatsScreen values the binary dispatches on are a jump table (BuildStats @0x841778, byte table @0x485e92) which is
// not reproduced here - the shell selects a category by this enum instead.
enum class StatsCategory : uint8_t {
    General,       // BuildGeneralStats 0x00842a4c
    Crime,         // BuildCrimeStats   0x00842e7c
    Combat,        // BuildCombatStats  0x00843968
    Vehicle,       // BuildVehicleStats 0x00843048
    Money,         // BuildMoneyStats   0x00843458
    // ---- scaffolded but not ported here (minigame / score screens); see pdastats.cpp notes ----
    Trading,       // BuildTradingStats 0x008436e8
    OddJob,        // BuildOddJobStats  0x00843b3c
    Races,         // BuildRaces        0x008453a4
    TimeTrials,    // BuildTimeTrials   0x00844028
    Rampages,      // BuildRampages     0x008449a8
    ShootingRange, // BuildShootingRange0x0084579c
    Count
};

// One built row, ready to draw. `value` is already resolved from Stats; `format` says how to render it; `labelId`
// is a StatsText GXT id the caller resolves to text. `available` is false for a counter the port does not track yet
// (the row is still shown, with a placeholder, to preserve the layout).
struct StatRow {
    uint16_t labelId = 0;           // StatsText GXT string id (observation: 0x531.. range)
    uint32_t value = 0;
    uint32_t value2 = 0;
    eStatFormat format = eStatFormat::Plain;
    uint8_t sprite = 0;             // medal sprite kind (0 = none)
    bool available = true;          // false => value is a placeholder, not tracked by the port
};

// The pause-stats screen heading GXT id for a category (cPauseStatsApp::SetTitle 0x00840250).
// Returns a StatsText GXT id; 0 if the category heading is not ported.
uint16_t PdaStats_TitleId(StatsCategory cat);

// Build the ordered rows for one category, pulling every value straight from the port's Stats exactly as the
// matching Build<Category>Stats routine reads cStatsManager::GetStatsData. `save` supplies the other save-game
// sections the General/Vehicle screens need (cams, safehouses, encounters, USJs); pass a default-constructed
// SaveProgress if unavailable (those rows then read 0, matching an empty save).
std::vector<StatRow> PdaStats_BuildGeneral(const Stats& s, const SaveProgress& save);
std::vector<StatRow> PdaStats_BuildCrime(const Stats& s);
std::vector<StatRow> PdaStats_BuildCombat(const Stats& s);
std::vector<StatRow> PdaStats_BuildVehicle(const Stats& s, const SaveProgress& save);
std::vector<StatRow> PdaStats_BuildMoney(const Stats& s);
// The minigame / score sub-screens (cPauseStatsApp::BuildSubStats 0x00842644 dispatch). Trading/OddJob are primary
// screens (BuildStats), the rest are sub screens; all take (s, save) for a uniform dispatch. Rows whose counter the
// port's Stats/SaveProgress does not own are emitted with available=false (a documented placeholder), the row layout
// and GXT label id preserved. Medal columns read SaveProgress.ambientMedal (GetMedalAwarded), so they stay real even
// when the row's value is a placeholder.
std::vector<StatRow> PdaStats_BuildTrading(const Stats& s, const SaveProgress& save);   // BuildTradingStats 0x008436e8
std::vector<StatRow> PdaStats_BuildOddJob(const Stats& s, const SaveProgress& save);     // BuildOddJobStats  0x00843b3c
std::vector<StatRow> PdaStats_BuildRaces(const Stats& s, const SaveProgress& save);       // BuildRaces        0x008453a4
std::vector<StatRow> PdaStats_BuildTimeTrials(const Stats& s, const SaveProgress& save);  // BuildTimeTrials   0x00844028
std::vector<StatRow> PdaStats_BuildRampages(const Stats& s, const SaveProgress& save);    // BuildRampages     0x008449a8
std::vector<StatRow> PdaStats_BuildShootingRange(const Stats& s, const SaveProgress& save);// BuildShootingRange 0x0084579c
std::vector<StatRow> PdaStats_Build(StatsCategory cat, const Stats& s, const SaveProgress& save);

// Format one row's value to a UTF-16 string, a faithful-enough subset of cPauseStatsApp::FormatValue (0x008462bc).
// `textProvider`, when set, resolves a StatsText/GlobalText/AppText id for the id-valued formats (13/11/15); when
// null those render as a neutral "#<id>" key. `isPhone` selects the metric/imperial distance unit as IsAPhone does.
std::u16string PdaStats_FormatValue(const StatRow& row,
                                    const std::function<std::u16string(uint16_t)>& textProvider = {},
                                    bool isPhone = true);

// A pixel rectangle in the PDA's 1024x768 content space that the shell gives the app to draw in.
struct ContentArea { float x = 45, y = 30, w = 464, h = 256; };   // cListBox ctor defaults (BuildStats @0x841778)

// The standalone component the Lane-1 PDA shell hosts. Shaped to plug into a cPdaApp-like interface:
//   void setCategory(StatsCategory)   - cPauseStatsApp::BuildStats: rebuild the row list / heading
//   void process(float dt)            - cPauseStatsApp::Process (0x00841f70): advance scroll momentum
//   bool input(Input)                 - category paging (swipe / widget bar) and row scrolling
//   void render(const ContentArea&)   - cPauseStatsApp::Render (0x008469ac): draw the visible rows
// The shell binds a text provider (StatsText GXT) and supplies the live Stats + SaveProgress.
class PdaStatsApp {
public:
    enum class Input { None, PrevCategory, NextCategory, ScrollUp, ScrollDown };

    void init(const Stats* stats, const SaveProgress* save,
              std::function<std::u16string(uint16_t)> textProvider = {});
    void setCategory(StatsCategory cat);
    StatsCategory category() const { return category_; }
    uint16_t titleId() const { return PdaStats_TitleId(category_); }
    const std::vector<StatRow>& rows() const { return rows_; }

    void process(float dt);
    bool input(Input in);
    void render(const ContentArea& area) const;   // uses Hud_Rect / Hud_Text16

    // The shell hands us gameplayHud's SS_AlwaysLoaded sheet so render() can draw the medal sprites a medal row
    // carries (cPauseStatsApp::AddStatToListBox 0x845fa8 -> cTaskBar::GetSpriteDataAllwaysLoaded). The sheet, its GL
    // texture and the PNG's size are owned by GameplayHud; we only reference them. Until this is set, medal drawing is
    // a no-op. Call with GameplayHud::alwaysLoaded(), alwaysLoadedTexture(), alwaysLoadedWidth(), alwaysLoadedHeight().
    void setMedalSheet(const SpriteSet* sheet, unsigned texture, int texW, int texH);

    // The SS_AlwaysLoaded sprite id a StatRow.sprite kind draws (AddStatToListBox 0x845fc4: 1->0x52, 2->0x54,
    // any other non-zero kind->0x53); 0 for kind 0 (no medal). Static + GL-free so tests can assert the mapping.
    static uint16_t medalSpriteId(uint8_t spriteKind);

    // Layout constants observed in BuildStats / AddStatToListBox (PDA content space, 1024x768).
    static constexpr float kRowHeight = 37.f;      // cListBox row pitch (phone: 0x25)
    static constexpr float kFontScale = 1.f;
    static constexpr int kFont = 7;                // cListBox font argument
    static constexpr uint32_t kRowColor = 0x19E5E5E5u;  // cPauseStatsApp::Render tint (RGBA)

private:
    void rebuild();
    const Stats* stats_ = nullptr;
    const SaveProgress* save_ = nullptr;
    std::function<std::u16string(uint16_t)> text_;
    std::vector<StatRow> rows_;
    StatsCategory category_ = StatsCategory::General;
    float scroll_ = 0.f;           // top visible row offset, in rows
    int firstVisible_ = 0;
    // SS_AlwaysLoaded medal sheet, borrowed from GameplayHud (not owned); null until setMedalSheet().
    const SpriteSet* medalSheet_ = nullptr;
    unsigned medalTexture_ = 0;
    int medalTexW_ = 0, medalTexH_ = 0;
};
