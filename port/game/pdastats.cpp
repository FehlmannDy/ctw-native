// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Gui::cPauseStatsApp, the pause-menu / PDA "Stats" application. See pdastats.h for the verified routine list.
//
// The Build<Category>Stats routines (0x00842a4c..0x00843b3c) each assemble a stack array of 28-byte sStat
// descriptors in display order, then loop calling cPauseStatsApp::AddStatToListBox (0x00845e44), which looks up the
// label GXT string (StatsText, biased by GetNumberOfGlobalStrings) and formats the value with FormatValue
// (0x008462bc). The row order and the counter each row reads were taken field-by-field from the disassembly; the
// StatsData offsets below cite the save-game layout mirrored by StatsData in stats.h.
//
// NO game text is baked in: every row carries only its StatsText GXT string id (observation: the ids are contiguous
// per screen, e.g. General 0x540.., Crime 0x568.., Combat 0x59c.., Vehicle 0x571.., Money 0x585..). The id is handed
// to a text provider at render time, or shown as a neutral "#<id>" key when none is bound.
#include "pdastats.h"
#include "hud.h"
#include "gfx/spriteset.h"
#include <glad/gl.h>

namespace {

// Pull a bit-packed counter out of one of StatsData's 64-bit words, matching the lsr/ubfx the binary uses.
inline uint32_t bits(uint64_t word, unsigned shift, unsigned width) {
    return (uint32_t)((word >> shift) & ((width >= 32) ? 0xFFFFFFFFull : ((1ull << width) - 1)));
}

StatRow row(uint16_t labelId, uint32_t value, eStatFormat fmt, bool available = true) {
    StatRow r;
    r.labelId = labelId;
    r.value = value;
    r.format = fmt;
    r.available = available;
    return r;
}

// A section-header row (sStat.format == 14, cPauseStatsApp::FormatValue index 14 "Identity": no value). The Build
// routines emit these as a title line; the GXT id is resolved by the text provider like any other label.
StatRow headerRow(uint16_t labelId) {
    StatRow r;
    r.labelId = labelId;
    r.format = eStatFormat::Identity;
    r.available = true;       // Identity renders an empty value string, so the row shows only its heading
    return r;
}

// A medal/score row for a minigame the port does not track: the value is a documented placeholder (available=false,
// rendered "--"), but the medal sprite still reads SaveProgress.ambientMedal via GetMedalAwarded (an int 0..3), which
// the port owns. `fmt` records the original sStat.format for layout fidelity even though the placeholder hides it.
StatRow medalRow(uint16_t labelId, eStatFormat fmt, int medal) {
    StatRow r;
    r.labelId = labelId;
    r.format = fmt;
    r.available = false;
    r.sprite = (uint8_t)(medal < 0 ? 0 : medal);
    return r;
}

// Draw one SS_AlwaysLoaded sprite by id, mirroring GameplayHud::sprite's half-res texel math (the shipped SS_* PNG is
// 2x the layout's pixel space, so layout x/y/w/h are doubled for the UVs). cTaskBar::GetSpriteDataAllwaysLoaded
// (0x007ca0c4) indexes the sheet by id (base + id*16), so the sprite id IS the def index. Coordinates are the PDA
// content space render() already draws its text in; the medals are not rotated or tinted (AddItemSprite scale 1.0).
void drawAlwaysLoadedSprite(const SpriteSet& sheet, unsigned texture, int texW, int texH,
                            int id, float x, float y, float scale) {
    if (!texture || texW <= 0 || texH <= 0) return;
    if (id < 0 || id >= (int)sheet.sprites().size()) return;
    const SpriteDef& d = sheet.sprites()[(size_t)id];
    const float w = d.w + 1.f, h = d.h + 1.f;              // cSprite::CalculateVerts includes the last stored pixel
    if ((d.x + w) * 2 > texW || (d.y + h) * 2 > texH) return;
    const float vx[4] = {-(float)d.offX, w - d.offX, w - d.offX, -(float)d.offX};
    const float vy[4] = {-(float)d.offY, -(float)d.offY, h - d.offY, h - d.offY};
    const float u[4]  = {d.x * 2.f, (d.x + w) * 2.f, (d.x + w) * 2.f, d.x * 2.f};
    const float v[4]  = {d.y * 2.f, d.y * 2.f, (d.y + h) * 2.f, (d.y + h) * 2.f};
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    glBegin(GL_QUADS);
    for (int i = 0; i < 4; ++i) {
        glTexCoord2f(u[i] / texW, v[i] / texH);
        glVertex2f(x + vx[i] * scale, y + vy[i] * scale);
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

} // namespace

uint16_t PdaStats_TitleId(StatsCategory cat) {
    // cPauseStatsApp::SetTitle (0x00840250): StatsText ids for the ported category headings.
    switch (cat) {
        case StatsCategory::General:  return 0x530;   // (Init sets the app heading to StatsText 0x530)
        case StatsCategory::Crime:    return 0x536;
        case StatsCategory::Combat:   return 0x53b;
        case StatsCategory::Vehicle:  return 0x534;
        case StatsCategory::Money:    return 0x537;
        // Sub-screen headings from the SetTitle byte jump table @0x485e7e (title = StatsText 0x532 + screen index;
        // BuildSubStats dispatches TimeTrials=7, Rampages=8, Races=9, ShootingRange=10).
        case StatsCategory::Races:         return 0x53b;   // screen 9
        case StatsCategory::TimeTrials:    return 0x539;   // screen 7
        case StatsCategory::Rampages:      return 0x53a;   // screen 8
        case StatsCategory::ShootingRange: return 0x53c;   // screen 10
        // Trading/OddJob are primary screens (BuildStats); their screen-index -> SetTitle id is not pinned here, so
        // the heading is left unresolved (0) rather than guessed. See report.
        default:                      return 0;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// BuildGeneralStats (0x00842a4c): 21 rows. Labels run StatsText 0x540.. with header rows at 0x54b/0x54d/0x54f/
// 0x551/0x63d. The percentage-complete and medal/criminal-rating header rows come from cSaveGameMgr and the
// cPauseStatsApp fields, which the port's Stats does not own; those rows are marked unavailable (placeholder).
// ---------------------------------------------------------------------------------------------------------------
std::vector<StatRow> PdaStats_BuildGeneral(const Stats& s, const SaveProgress& save) {
    const StatsData& d = s.data();
    std::vector<StatRow> v;
    v.reserve(21);
    // cSaveGameMgr::SumUpPercentageComplete - not tracked by Stats.
    v.push_back(row(0x540, 0, eStatFormat::Plain, /*available=*/false));              // % complete
    v.push_back(row(0x541, s.missionsPassed(),  eStatFormat::Plain));                 // [0xC0]>>44 & 0xFF
    v.push_back(row(0x542, s.missionsFailed(),  eStatFormat::Plain));                 // [0xC0]>>20 & 0xFFF
    v.push_back(row(0x543, s.missionRetries(),  eStatFormat::Plain));                 // wordD8 >>12
    v.push_back(row(0x544, s.timesBusted(),     eStatFormat::Plain));                 // [0xBE] & 0xFFF
    v.push_back(row(0x545, s.timesWasted(),     eStatFormat::Plain));                 // [0xC0] & 0xFFF
    v.push_back(row(0x546, s.fishesFed(),       eStatFormat::Plain));                 // [0xC4] & 0xFFF (wordC0 bits32-43)
    v.push_back(row(0x547, d.peopleKilled,      eStatFormat::Plain));                 // [0x70]
    v.push_back(row(0x548, s.currentPlayingTime(), eStatFormat::TimeElapsed));        // cStatsManager +0
    v.push_back(row(0x549, d.totalPlayingTime,  eStatFormat::TimeElapsed));           // [0x04]
    v.push_back(row(0x54a, s.daysPassed(),      eStatFormat::Plain));                 // [0xC0]>>52
    // Header/derived rows (immediate label ids). Criminal rating & media value map onto Stats; cams / safehouses /
    // encounters come from SaveProgress; the exact row<->id pairing of these Calc* rows is reconstructed.
    v.push_back(row(0x54b, (uint32_t)d.criminalRating, eStatFormat::StatsTextId));    // stored rating -> text id
    v.push_back(row(0x54d, (uint32_t)s.calcMediaValue(0),    eStatFormat::Plain, false)); // needs live wanted level
    v.push_back(row(0x54f, (uint32_t)Stats::calcCamsDestroyed(save),        eStatFormat::Plain));
    v.push_back(row(0x551, (uint32_t)Stats::calcRandomEncountersFound(save),eStatFormat::Plain));
    v.push_back(row(0x552, (uint32_t)Stats::calcSafehousesOwned(save),      eStatFormat::Plain));
    v.push_back(row(0x553, s.timesCheated(),    eStatFormat::Plain));                 // [0xC8] & 0xFFF
    v.push_back(row(0x554, s.timesSaved(),      eStatFormat::Plain));                 // [0xC8]>>20 & 0xFFF
    v.push_back(row(0x63d, d.unknown08,         eStatFormat::Plain, false));          // [0x08] purpose unconfirmed
    v.push_back(row(0x550, d.peopleBurntOut,    eStatFormat::Plain));                 // [0x6E]
    v.push_back(row(0x54e, d.lionsCollected,    eStatFormat::Plain));                 // [0x6C]
    return v;
}

// ---------------------------------------------------------------------------------------------------------------
// BuildCrimeStats (0x00842e7c): 7 rows, labels StatsText 0x568..
// ---------------------------------------------------------------------------------------------------------------
std::vector<StatRow> PdaStats_BuildCrime(const Stats& s) {
    const StatsData& d = s.data();
    std::vector<StatRow> v;
    v.push_back(row(0x568, d.vehiclesStolen,         eStatFormat::Plain));            // [0x76]
    v.push_back(row(0x569, d.wantedStarsAttained,    eStatFormat::Plain));            // [0x78]
    v.push_back(row(0x56a, d.wantedStarsEvaded,      eStatFormat::Plain));            // [0x7A]
    v.push_back(row(0x56b, d.longestPoliceChase,     eStatFormat::TimeElapsed));      // [0x7E]
    v.push_back(row(0x56c, d.longestChaseWith6Stars, eStatFormat::TimeElapsed));      // [0x80]
    v.push_back(row(0x56d, s.calcTotalGangMembersWasted(), eStatFormat::Plain));      // gangKills
    v.push_back(row(0x56f, d.criminalsKilled,        eStatFormat::Plain));            // [0x7C]
    return v;
}

// ---------------------------------------------------------------------------------------------------------------
// BuildCombatStats (0x00843968): 7 rows, labels StatsText 0x59c..
// ---------------------------------------------------------------------------------------------------------------
std::vector<StatRow> PdaStats_BuildCombat(const Stats& s) {
    const StatsData& d = s.data();
    std::vector<StatRow> v;
    v.push_back(row(0x59c, d.bulletsFired,  eStatFormat::Plain));                     // [0x54]
    v.push_back(row(0x59d, d.bulletsThatHit, eStatFormat::Plain));                    // [0x58]
    v.push_back(row(0x59e, (uint32_t)s.calcAccuracy(), eStatFormat::FixedQ12));       // CalcAccuracy (percent, Q12)
    v.push_back(row(0x5a0, d.meleeKills,    eStatFormat::Plain));                     // [0x8A]
    v.push_back(row(0x5a1, s.explosions(),  eStatFormat::Plain));                     // wordD0 >>52
    v.push_back(row(0x5a2, s.weaponsPickedUp(), eStatFormat::Plain));                 // wordD8 & 0xFFF
    v.push_back(row(0x59f, (uint32_t)s.calcFavouriteWeapon(), eStatFormat::StatsTextId)); // weapon-type name id
    return v;
}

// ---------------------------------------------------------------------------------------------------------------
// BuildVehicleStats (0x00843048): 21 rows, labels StatsText 0x571.. with header rows 0x57c/0x57e/0x580/0x582/0x584.
// Distances are stored in 1/16 units; the binary shifts them >>4 before formatting.
// ---------------------------------------------------------------------------------------------------------------
std::vector<StatRow> PdaStats_BuildVehicle(const Stats& s, const SaveProgress& save) {
    const StatsData& d = s.data();
    std::vector<StatRow> v;
    v.reserve(21);
    v.push_back(row(0x571, d.carsAndBikesDestroyed, eStatFormat::Plain));             // [0x86]
    v.push_back(row(0x572, d.boatsDestroyed,        eStatFormat::Plain));             // [0x88]
    v.push_back(row(0x573, d.distance[0] >> 4, eStatFormat::Distance));               // on foot [0x14]
    v.push_back(row(0x574, d.distance[1] >> 4, eStatFormat::Distance));               // swum   [0x18]
    v.push_back(row(0x575, d.distance[2] >> 4, eStatFormat::Distance));               // by car [0x1C]
    v.push_back(row(0x576, d.distance[3] >> 4, eStatFormat::Distance));               // by bike[0x20]
    v.push_back(row(0x577, d.distance[4] >> 4, eStatFormat::Distance));               // by boat[0x24]
    v.push_back(row(0x578, (uint32_t)s.calcTotalDistanceTravelled() >> 4, eStatFormat::Distance));
    v.push_back(row(0x579, bits(d.wordC8, 32, 12), eStatFormat::Distance));           // longest wheelie [0xCC]&0xFFF
    v.push_back(row(0x57c, bits(d.wordC8, 52, 12), eStatFormat::Distance));           // longest stoppie [0xC8]>>52
    v.push_back(row(0x57a, bits(d.wordD0, 0, 12),  eStatFormat::Distance));           // longest 2-wheel [0xD0]&0xFFF
    v.push_back(row(0x57e, bits(d.wordD0, 20, 12), eStatFormat::Distance));           // farthest jump   [0xD0]>>20
    v.push_back(row(0x57f, bits(d.wordD0, 32, 12), eStatFormat::Distance));           // highest jump    [0xD0]>>32 (assumption: 21st row)
    v.push_back(row(0x57b, bits(d.wordD0, 12, 8),  eStatFormat::Plain));              // most flips      [0xD0]>>12 &0xFF
    v.push_back(row(0x580, bits(d.wordD0, 44, 8),  eStatFormat::Plain));              // most spin       [0xD0] bits44-51
    v.push_back(row(0x57d, (uint32_t)Stats::calcUSJsCompleted(save), eStatFormat::Plain));
    v.push_back(row(0x582, (uint32_t)s.calcMostFavRadioStation(),  eStatFormat::StatsTextId));
    v.push_back(row(0x581, (uint32_t)s.calcLeastFavRadioStation(), eStatFormat::StatsTextId));
    v.push_back(row(0x584, d.longestTimeInVehicle, eStatFormat::TimeElapsed));        // [0x5C] (seconds)
    v.push_back(row(0x583, d.peopleBurntOut, eStatFormat::Plain));                    // [0x72]
    v.push_back(row(0x585, d.nearMisses,     eStatFormat::Plain));                    // [0x74]
    return v;
}

// ---------------------------------------------------------------------------------------------------------------
// BuildMoneyStats (0x00843458): 12 rows, labels StatsText 0x585.., every value a money amount ($).
// ---------------------------------------------------------------------------------------------------------------
std::vector<StatRow> PdaStats_BuildMoney(const Stats& s) {
    const StatsData& d = s.data();
    std::vector<StatRow> v;
    v.reserve(12);
    v.push_back(row(0x585, d.spentOnWeapons,    eStatFormat::Money));                 // [0x28]
    v.push_back(row(0x586, d.spentOnPayNSpray,  eStatFormat::Money));                 // [0x2C]
    v.push_back(row(0x587, d.spentOnProperty,   eStatFormat::Money));                 // [0x30]
    v.push_back(row(0x588, d.lostOnStreetRaces, eStatFormat::Money));                 // [0x34]
    v.push_back(row(0x589, d.spentOnBribingCops,eStatFormat::Money));                 // [0x38]
    v.push_back(row(0x58a, d.spentOnHealthCare, eStatFormat::Money));                 // [0x3C]
    v.push_back(row(0x58b, (uint32_t)d.madeFromTrading, eStatFormat::Money));         // [0x60]
    v.push_back(row(0x58c, d.madeFromActivities,eStatFormat::Money));                 // [0x40]
    v.push_back(row(0x58d, d.madeFromTaxiTips,  eStatFormat::Money));                 // [0x44]
    v.push_back(row(0x58e, d.foundOnTheStreet,  eStatFormat::Money));                 // [0x48]
    v.push_back(row(0x590, (uint32_t)d.spentOnScratchCards, eStatFormat::Money));     // [0x64]
    v.push_back(row(0x58f, (uint32_t)d.madeOnScratchCards,  eStatFormat::Money));     // [0x68]
    return v;
}

// ---------------------------------------------------------------------------------------------------------------
// BuildTradingStats (0x008436e8): 12 rows, labels StatsText 0x591..0x59b then 0x610. Every counter maps onto the
// port's Stats/SaveProgress, so this screen is ported in full. The value-of-goods / average / drugs-confiscated rows
// use the "$" money format (the same rodata format constant @0x4685d0 that BuildMoneyStats uses -> eStatFormat::Money).
// The dealers / unique-commodities rows read SaveProgress (CalculateNumDealersFound / CalculateUniqueCommoditiesOwned).
// ---------------------------------------------------------------------------------------------------------------
std::vector<StatRow> PdaStats_BuildTrading(const Stats& s, const SaveProgress& save) {
    const StatsData& d = s.data();
    std::vector<StatRow> v;
    v.reserve(12);
    v.push_back(row(0x591, d.tradesDone & 0xFFFFF, eStatFormat::Plain));                    // [0xDC] & 0xFFFFF
    v.push_back(row(0x592, (uint32_t)Stats::calculateNumDealersFound(save), eStatFormat::Plain));
    v.push_back(row(0x593, bits(d.wordC8, 12, 8), eStatFormat::Plain));                     // shipments robbed [0xC8]>>12
    v.push_back(row(0x594, bits(d.wordC8, 44, 8), eStatFormat::Plain));                     // warehouse raids  [0xC8]>>44
    v.push_back(row(0x595, d.valueOfGoodsSold,    eStatFormat::Money));                     // [0x4C]
    v.push_back(row(0x596, d.unitsOfGoodsSold,    eStatFormat::Plain));                     // [0x82]
    v.push_back(row(0x597, s.calcAverageValueOfGoodsSold(),   eStatFormat::Money));
    v.push_back(row(0x598, d.valueOfGoodsBought,  eStatFormat::Money));                     // [0x50]
    v.push_back(row(0x599, d.unitsOfGoodsBought,  eStatFormat::Plain));                     // [0x84]
    v.push_back(row(0x59a, s.calcAverageValueOfGoodsBought(), eStatFormat::Money));
    v.push_back(row(0x59b, (uint32_t)Stats::calculateUniqueCommoditiesOwned(save), eStatFormat::Plain));
    v.push_back(row(0x610, s.totalValueDrugsConfiscated(), eStatFormat::Money));            // TotalValueDrugsConfiscated
    return v;
}

// ---------------------------------------------------------------------------------------------------------------
// BuildOddJobStats (0x00843b3c): 13 rows. Labels mix GlobalText (0x336/0x338/0x339/0x33f/0x340/0x342/0x343) and
// StatsText (0x5be/0x5bf/0x5c1/0x63b/0x63c/0x63e) ids - both resolved by the text provider. Rows 5 and 10 are section
// headers (Identity). The medal sprites read SaveGameImpl::GetMedalAwarded(mission) == SaveProgress.ambientMedal, so
// they are real; the row VALUES come from SaveGameImpl odd-job score/time routines the port has no field for, so they
// are placeholders - EXCEPT the tattoos row (GetTattoosDone == SaveProgress.tattooBestScore), which is real.
// ---------------------------------------------------------------------------------------------------------------
std::vector<StatRow> PdaStats_BuildOddJob(const Stats& s, const SaveProgress& save) {
    (void)s;
    std::vector<StatRow> v;
    v.reserve(13);
    v.push_back(medalRow(0x63e, eStatFormat::Plain,       Stats::medalAwarded(save, 0x00))); // TaxiTipRun
    v.push_back(medalRow(0x336, eStatFormat::TwoDecimal,  Stats::medalAwarded(save, 0x01))); // VigilanteAvgKillRate
    v.push_back(medalRow(0x338, eStatFormat::TwoDecimal,  Stats::medalAwarded(save, 0x02))); // ParamedicAvgKillRate
    v.push_back(medalRow(0x339, eStatFormat::Plain,       Stats::medalAwarded(save, 0x03))); // FireLevel
    v.push_back(medalRow(0x33f, eStatFormat::MinutesSeconds, Stats::medalAwarded(save, 0x17)));// TankBestTime
    v.push_back(headerRow(0x340));
    v.push_back(medalRow(0x63b, eStatFormat::Plain,       Stats::medalAwarded(save, 0x18))); // ChineseLevel(0)
    v.push_back(medalRow(0x63c, eStatFormat::Plain,       Stats::medalAwarded(save, 0x19))); // ChineseLevel(1)
    { StatRow r = row(0x5c1, Stats::tattoosDone(save), eStatFormat::Plain);                   // GetTattoosDone (real)
      r.sprite = (uint8_t)Stats::medalAwarded(save, 0x54); v.push_back(r); }
    v.push_back(medalRow(0x343, eStatFormat::Plain,       Stats::medalAwarded(save, 0x21))); // HobotronBestScore
    v.push_back(headerRow(0x342));
    v.push_back(medalRow(0x5bf, eStatFormat::TwoDecimal,  Stats::medalAwarded(save, 0x1f))); // PackageBestTime(0)
    v.push_back(medalRow(0x5be, eStatFormat::MinutesSeconds, Stats::medalAwarded(save, 0x20)));// PackageBestTime(1)
    return v;
}

// ---------------------------------------------------------------------------------------------------------------
// BuildRaces (0x008453a4): 11 rows = 3 section headers (GlobalText 0x357/0x358/0x359, Identity) and 8 race rows.
// Ported in full: each race row's time is GetBestRaceTime(id) == Stats::bestRaceTime(save,id) and its medal is
// GetMedalAwarded(id) == SaveProgress.ambientMedal[id]. Race ids 0x4C..0x53 appear in the game's menu order, which is
// not sorted by label (0x4C->0x35a, 0x4D->0x35c, 0x4E->0x35b). Times use TimeElapsed (0x800000000 format constant).
// ---------------------------------------------------------------------------------------------------------------
namespace {
StatRow raceRow(uint16_t labelId, const SaveProgress& save, int raceId) {
    StatRow r;
    r.labelId = labelId;
    r.value = Stats::bestRaceTime(save, raceId);        // GetBestRaceTime
    r.format = eStatFormat::TimeElapsed;
    r.available = true;
    r.sprite = (uint8_t)Stats::medalAwarded(save, raceId);
    return r;
}
} // namespace
std::vector<StatRow> PdaStats_BuildRaces(const Stats& s, const SaveProgress& save) {
    (void)s;
    std::vector<StatRow> v;
    v.reserve(11);
    v.push_back(headerRow(0x359));
    v.push_back(raceRow(0x35a, save, 0x4c));
    v.push_back(raceRow(0x35c, save, 0x4d));
    v.push_back(raceRow(0x35b, save, 0x4e));
    v.push_back(headerRow(0x357));
    v.push_back(raceRow(0x35d, save, 0x4f));
    v.push_back(raceRow(0x35e, save, 0x50));
    v.push_back(raceRow(0x35f, save, 0x51));
    v.push_back(headerRow(0x358));
    v.push_back(raceRow(0x360, save, 0x52));
    v.push_back(raceRow(0x361, save, 0x53));
    return v;
}

// ---------------------------------------------------------------------------------------------------------------
// BuildTimeTrials (0x00844028): 31 rows. Five course sections, each a header (GlobalText 0x33a..0x33e, Identity)
// followed by four lap rows (StatsText 0x5bd) whose value/value2 are GetTTCourseTime / GetTTCourseBestLapTime
// (TimeElapsedRange) - plus a PCJ section of seven rows (0x34d..0x356, PCJBestTime, TimeElapsed). The medals
// (GetMedalAwarded 0x04..0x16 for the courses, 0x45..0x4b for PCJ) read SaveProgress.ambientMedal and are real; the
// time VALUES come from SaveGameImpl routines the port has no field for, so every time is a placeholder.
// ---------------------------------------------------------------------------------------------------------------
std::vector<StatRow> PdaStats_BuildTimeTrials(const Stats& s, const SaveProgress& save) {
    (void)s;
    std::vector<StatRow> v;
    v.reserve(31);
    static const uint16_t kCourseHdr[5] = {0x33a, 0x33b, 0x33c, 0x33d, 0x33e};
    static const uint8_t  kLaps[5]      = {4, 4, 4, 4, 3};       // the last course has three timed laps
    int medal = 0x04;
    for (int c = 0; c < 5; ++c) {
        v.push_back(headerRow(kCourseHdr[c]));
        for (int lap = 0; lap < kLaps[c]; ++lap)
            v.push_back(medalRow(0x5bd, eStatFormat::TimeElapsedRange, Stats::medalAwarded(save, medal++)));
    }
    static const uint16_t kPcj[7] = {0x34d, 0x34e, 0x34f, 0x350, 0x352, 0x353, 0x356};
    for (int i = 0; i < 7; ++i)
        v.push_back(medalRow(kPcj[i], eStatFormat::TimeElapsed, Stats::medalAwarded(save, 0x45 + i)));
    return v;
}

// ---------------------------------------------------------------------------------------------------------------
// BuildRampages (0x008449a8): 37 rows = a header (GlobalText 0x372, Identity) + 30 rampage rows (StatsText
// 0x618..0x635, GetRampageScore, medals 0x22..0x3f) + a second header (0x373) + 5 turret-rampage rows (0x636..0x63a,
// GetTurretRampageScore, medals 0x40..0x44). The medals read SaveProgress.ambientMedal and are real; the scores come
// from SaveGameImpl::GetRampageScore / GetTurretRampageScore, which the port has no field for, so they are placeholders.
// ---------------------------------------------------------------------------------------------------------------
std::vector<StatRow> PdaStats_BuildRampages(const Stats& s, const SaveProgress& save) {
    (void)s;
    std::vector<StatRow> v;
    v.reserve(37);
    v.push_back(headerRow(0x372));
    int medal = 0x22;
    for (int i = 0; i < 30; ++i)                                                   // GetRampageScore(0..29)
        v.push_back(medalRow((uint16_t)(0x618 + i), eStatFormat::Plain, Stats::medalAwarded(save, medal++)));
    v.push_back(headerRow(0x373));
    for (int i = 0; i < 5; ++i)                                                    // GetTurretRampageScore(0..4)
        v.push_back(medalRow((uint16_t)(0x636 + i), eStatFormat::Plain, Stats::medalAwarded(save, medal++)));
    return v;
}

// ---------------------------------------------------------------------------------------------------------------
// BuildShootingRange (0x0084579c): 5 rows, labels StatsText 0x609..0x60d, built inline (no header). Each value is
// SaveGameImpl::ShootScore(0..4) - a shooting-range score the port has no field for (placeholder) - with the medal
// GetMedalAwarded(0x1a..0x1e) == SaveProgress.ambientMedal, which is real.
// ---------------------------------------------------------------------------------------------------------------
std::vector<StatRow> PdaStats_BuildShootingRange(const Stats& s, const SaveProgress& save) {
    (void)s;
    std::vector<StatRow> v;
    v.reserve(5);
    for (int i = 0; i < 5; ++i)                                                    // ShootScore(i), medal 0x1a+i
        v.push_back(medalRow((uint16_t)(0x609 + i), eStatFormat::Plain, Stats::medalAwarded(save, 0x1a + i)));
    return v;
}

std::vector<StatRow> PdaStats_Build(StatsCategory cat, const Stats& s, const SaveProgress& save) {
    switch (cat) {
        case StatsCategory::General:       return PdaStats_BuildGeneral(s, save);
        case StatsCategory::Crime:         return PdaStats_BuildCrime(s);
        case StatsCategory::Combat:        return PdaStats_BuildCombat(s);
        case StatsCategory::Vehicle:       return PdaStats_BuildVehicle(s, save);
        case StatsCategory::Money:         return PdaStats_BuildMoney(s);
        case StatsCategory::Trading:       return PdaStats_BuildTrading(s, save);
        case StatsCategory::OddJob:        return PdaStats_BuildOddJob(s, save);
        case StatsCategory::Races:         return PdaStats_BuildRaces(s, save);
        case StatsCategory::TimeTrials:    return PdaStats_BuildTimeTrials(s, save);
        case StatsCategory::Rampages:      return PdaStats_BuildRampages(s, save);
        case StatsCategory::ShootingRange: return PdaStats_BuildShootingRange(s, save);
        default:                           return {};
    }
}

// ---------------------------------------------------------------------------------------------------------------
// FormatValue (0x008462bc): a faithful-enough subset. The id-valued formats defer to the text provider (so no game
// strings are baked); the numeric formats mirror the arithmetic the binary applies before printing.
// ---------------------------------------------------------------------------------------------------------------
static std::u16string u16(const std::string& a) { return std::u16string(a.begin(), a.end()); }

static std::string decs(uint32_t n) { return std::to_string(n); }

std::u16string PdaStats_FormatValue(const StatRow& r,
                                    const std::function<std::u16string(uint16_t)>& textProvider,
                                    bool isPhone) {
    if (!r.available) return u16("--");   // placeholder for a counter the port does not track
    switch (r.format) {
        case eStatFormat::Money:
            return u16("$" + decs(r.value));
        case eStatFormat::FixedQ12: {                      // value / 4096, two decimals (accuracy %)
            uint32_t whole = r.value >> 12;
            uint32_t frac  = (uint32_t)(((uint64_t)(r.value & 0xFFF) * 100) >> 12);
            std::string f = decs(frac);
            if (f.size() < 2) f = "0" + f;
            return u16(decs(whole) + "." + f + "%");
        }
        case eStatFormat::Distance: {                      // 1-unit values; metres (phone) vs feet (0x846404)
            return u16(decs(r.value) + (isPhone ? "m" : "ft"));
        }
        case eStatFormat::TwoDecimal: {
            std::string f = decs(r.value % 100);
            if (f.size() < 2) f = "0" + f;
            return u16(decs(r.value / 100) + "." + f);
        }
        case eStatFormat::Thousands:
            return u16(decs(r.value / 1000));
        case eStatFormat::MinutesSeconds: {
            uint32_t m = r.value / 60, sec = r.value % 60;
            std::string ss = decs(sec); if (ss.size() < 2) ss = "0" + ss;
            return u16(decs(m) + ":" + ss);
        }
        case eStatFormat::HoursMinSec:
        case eStatFormat::TimeElapsed: {                   // cCounter::TimeElapsed: seconds -> H:MM:SS
            uint32_t t = r.value, h = t / 3600, m = (t / 60) % 60, sec = t % 60;
            std::string ms = decs(m), ss = decs(sec);
            if (ms.size() < 2) ms = "0" + ms;
            if (ss.size() < 2) ss = "0" + ss;
            return u16(decs(h) + ":" + ms + ":" + ss);
        }
        case eStatFormat::Ratio:
            return u16(decs(r.value) + " / " + decs(r.value2));
        case eStatFormat::TimeElapsedRange: {              // TimeElapsed(value) ".." TimeElapsed(value2)
            StatRow a = r; a.format = eStatFormat::TimeElapsed; a.value2 = 0;
            StatRow b = r; b.format = eStatFormat::TimeElapsed; b.value = r.value2; b.value2 = 0;
            return PdaStats_FormatValue(a, textProvider, isPhone) + u16("..") +
                   PdaStats_FormatValue(b, textProvider, isPhone);
        }
        case eStatFormat::StatsTextId:
        case eStatFormat::GlobalTextId:
        case eStatFormat::AppTextId:
            if (textProvider) return textProvider((uint16_t)r.value);
            return u16("#" + decs(r.value));               // neutral key: the GXT lookup is a run-time dependency
        case eStatFormat::Identity:
            return u16("");
        case eStatFormat::Plain:
        case eStatFormat::CustomPercent:
        default:
            return u16(decs(r.value));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// PdaStatsApp: the hostable component.
// ---------------------------------------------------------------------------------------------------------------
void PdaStatsApp::init(const Stats* stats, const SaveProgress* save,
                       std::function<std::u16string(uint16_t)> textProvider) {
    stats_ = stats;
    save_ = save;
    text_ = std::move(textProvider);
    rebuild();
}

void PdaStatsApp::rebuild() {
    static const SaveProgress kEmpty{};
    if (stats_) rows_ = PdaStats_Build(category_, *stats_, save_ ? *save_ : kEmpty);
    else rows_.clear();
    firstVisible_ = 0;
    scroll_ = 0.f;
}

void PdaStatsApp::setCategory(StatsCategory cat) {
    if (cat == category_ || cat >= StatsCategory::Count) return;
    category_ = cat;
    rebuild();   // cPauseStatsApp::BuildStats: tear down and rebuild the list box + heading
}

void PdaStatsApp::process(float) {
    // cPauseStatsApp::Process (0x00841f70) advances cScrollMomentum and the gamepad cursor; with a static list the
    // visible window is derived in render(). Nothing stateful to step without live input here.
}

bool PdaStatsApp::input(Input in) {
    switch (in) {
        case Input::PrevCategory:
            if (category_ != StatsCategory::General) { setCategory((StatsCategory)((int)category_ - 1)); return true; }
            return false;
        case Input::NextCategory:
            if ((int)category_ + 1 < (int)StatsCategory::Count) { setCategory((StatsCategory)((int)category_ + 1)); return true; }
            return false;
        case Input::ScrollUp:
            if (firstVisible_ > 0) { --firstVisible_; return true; }
            return false;
        case Input::ScrollDown:
            if (firstVisible_ + 1 < (int)rows_.size()) { ++firstVisible_; return true; }
            return false;
        default:
            return false;
    }
}

void PdaStatsApp::setMedalSheet(const SpriteSet* sheet, unsigned texture, int texW, int texH) {
    medalSheet_ = sheet;
    medalTexture_ = texture;
    medalTexW_ = texW;
    medalTexH_ = texH;
}

uint16_t PdaStatsApp::medalSpriteId(uint8_t spriteKind) {
    // cPauseStatsApp::AddStatToListBox (0x845fc4..0x845fd8): sprite==1 -> 0x52, sprite==2 -> 0x54, else 0x53.
    if (spriteKind == 0) return 0;
    if (spriteKind == 1) return 0x52;
    if (spriteKind == 2) return 0x54;
    return 0x53;
}

void PdaStatsApp::render(const ContentArea& area) const {
    // cPauseStatsApp::Render (0x008469ac): draw the list box rows. Here we draw directly with the port's HUD text
    // primitives, laying rows out on the cListBox grid (row pitch kRowHeight, font kFont).
    const int maxRows = (int)(area.h / kRowHeight);
    const float labelX = area.x + 4.f;
    const float valueX = area.x + area.w - 4.f;   // value right-aligned (AddStatToListBox places it at the right)
    for (int i = 0; i + firstVisible_ < (int)rows_.size() && i < maxRows; ++i) {
        const StatRow& r = rows_[firstVisible_ + i];
        const float y = area.y + i * kRowHeight;
        std::u16string label = text_ ? text_(r.labelId) : (u"#" + u16(std::to_string(r.labelId)));
        std::u16string value = PdaStats_FormatValue(r, text_, /*isPhone=*/true);
        Hud_Text16(labelX, y, kFontScale, kRowColor, label);
        // Right-align the value by nudging left ~8px per glyph (the port measures the sprite; this approximates).
        Hud_Text16(valueX - value.size() * 8.f, y, kFontScale, kRowColor, value);
        // cPauseStatsApp::AddStatToListBox (0x845fa8): a medal row (sprite != 0) also draws an SS_AlwaysLoaded sprite
        // by id. The original places it at row-x 0x12c (300), or 0xd2 (210) for the two-time range format, at cFixed
        // scale 1.0. The original content row is 0x1e0 (480) px wide; here we place it proportionally in the port's
        // content width (the row text is likewise laid out approximately) and nudge it a couple px below the row top
        // (the original's small y offset). Guarded on the shell having supplied the sheet; otherwise a no-op.
        if (r.sprite != 0 && medalSheet_) {
            const float kSrcRowWidth = 480.f;   // AddStatToListBox value-text width arg (0x1e0)
            const float medalSrcX = (r.format == eStatFormat::TimeElapsedRange) ? 210.f : 300.f;
            const float medalX = area.x + medalSrcX * (area.w / kSrcRowWidth);
            drawAlwaysLoadedSprite(*medalSheet_, medalTexture_, medalTexW_, medalTexH_,
                                   medalSpriteId(r.sprite), medalX, y + 2.f, kFontScale);
        }
    }
}
