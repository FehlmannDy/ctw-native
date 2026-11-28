// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
// Tests for PdaStatsApp (a port of Gui::cPauseStatsApp): row order, counter->Stats mapping and value formatting.
// Synthetic data only; no game assets.
#include "../port/game/pdastats.h"
#include <cassert>
#include <cstdio>
#include <string>

static std::string narrow(const std::u16string& s) {
    std::string o;
    for (char16_t c : s) o += (c < 128) ? (char)c : '?';
    return o;
}

int main() {
    Stats s;
    SaveProgress save;
    StatsData& d = s.data();

    // --- Money screen: 12 rows, all money, first row = spentOnWeapons ([0x28]) ---
    d.spentOnWeapons = 1234;
    d.madeOnScratchCards = 50;
    auto money = PdaStats_BuildMoney(s);
    assert(money.size() == 12);
    assert(money[0].labelId == 0x585);
    assert(money[0].value == 1234);
    assert(money[0].format == eStatFormat::Money);
    assert(money.back().value == 50);                       // madeOnScratchCards is the last row
    assert(narrow(PdaStats_FormatValue(money[0])) == "$1234");

    // --- Combat screen: 7 rows; bullets fired/hit, accuracy %, favourite weapon is a text id ---
    d.bulletsFired = 200;
    d.bulletsThatHit = 100;
    d.meleeKills = 7;
    auto combat = PdaStats_BuildCombat(s);
    assert(combat.size() == 7);
    assert(combat[0].labelId == 0x59c && combat[0].value == 200);
    assert(combat[1].value == 100);
    assert(combat[2].format == eStatFormat::FixedQ12);      // accuracy
    assert(combat[3].value == 7);                           // melee kills
    assert(combat.back().format == eStatFormat::StatsTextId); // favourite weapon = StatsText id, deferred to provider
    // Text id with no provider renders a neutral key, never a baked string.
    assert(narrow(PdaStats_FormatValue(combat.back()))[0] == '#');

    // --- Crime screen: 7 rows; criminals killed is the last ---
    d.vehiclesStolen = 9;
    d.criminalsKilled = 3;
    auto crime = PdaStats_BuildCrime(s);
    assert(crime.size() == 7);
    assert(crime[0].labelId == 0x568 && crime[0].value == 9);
    assert(crime.back().value == 3);

    // --- Vehicle screen: 21 rows; distances come straight from StatsData (1/16 units, shifted >>4) ---
    d.distance[2] = 160;                                    // by car; 160/16 = 10 units
    auto veh = PdaStats_BuildVehicle(s, save);
    assert(veh.size() == 21);
    assert(veh[0].labelId == 0x571);                        // cars and bikes destroyed
    assert(veh[4].value == 10 && veh[4].format == eStatFormat::Distance);   // by car row

    // --- General screen: 21 rows; percentage-complete is a documented placeholder (not tracked) ---
    auto gen = PdaStats_BuildGeneral(s, save);
    assert(gen.size() == 21);
    assert(gen[0].available == false);                      // SumUpPercentageComplete not ported
    assert(narrow(PdaStats_FormatValue(gen[0])) == "--");

    // --- Format sanity ---
    StatRow t; t.format = eStatFormat::TimeElapsed; t.value = 3661;   // 1h 1m 1s
    assert(narrow(PdaStats_FormatValue(t)) == "1:01:01");

    // --- Trading screen: 12 rows, all counters map onto Stats/SaveProgress (ported in full) ---
    d.tradesDone = 5;                 // [0xDC] & 0xFFFFF
    d.valueOfGoodsSold = 777;         // [0x4C]
    d.drugsConfiscated = 9000;        // TotalValueDrugsConfiscated
    auto trading = PdaStats_BuildTrading(s, save);
    assert(trading.size() == 12);
    assert(trading[0].labelId == 0x591 && trading[0].value == 5 && trading[0].available);
    assert(trading[4].labelId == 0x595 && trading[4].value == 777 && trading[4].format == eStatFormat::Money);
    assert(trading.back().labelId == 0x610 && trading.back().format == eStatFormat::Money);
    assert(narrow(PdaStats_FormatValue(trading.back())) == "$9000");

    // --- Races screen: 11 rows = 3 Identity headers + 8 race rows; times + medals are real (SaveProgress) ---
    save.raceBestTime[0] = 120;       // race id 0x4C -> raceBestTime[0]
    save.ambientMedal[0x4c] = 3;      // gold medal for race 0x4C
    auto races = PdaStats_BuildRaces(s, save);
    assert(races.size() == 11);
    assert(races[0].format == eStatFormat::Identity && narrow(PdaStats_FormatValue(races[0])).empty());
    assert(races[1].value == 120 && races[1].format == eStatFormat::TimeElapsed && races[1].sprite == 3);
    assert(narrow(PdaStats_FormatValue(races[1])) == "0:02:00");

    // --- OddJob screen: 13 rows; values are placeholders except tattoos, medals come from SaveProgress ---
    save.tattooBestScore = 42;        // GetTattoosDone (real)
    save.ambientMedal[0] = 2;         // TaxiTipRun medal (silver)
    auto oddjob = PdaStats_BuildOddJob(s, save);
    assert(oddjob.size() == 13);
    assert(oddjob[0].available == false && oddjob[0].sprite == 2);        // placeholder value, real medal
    assert(narrow(PdaStats_FormatValue(oddjob[0])) == "--");
    assert(oddjob[5].format == eStatFormat::Identity);                    // section header
    assert(oddjob[8].available == true && oddjob[8].value == 42 && oddjob[8].labelId == 0x5c1); // tattoos (real)

    // --- TimeTrials screen: 31 rows; a header, then lap rows with a two-time range format (placeholders) ---
    auto tt = PdaStats_BuildTimeTrials(s, save);
    assert(tt.size() == 31);
    assert(tt[0].format == eStatFormat::Identity);
    assert(tt[1].available == false && tt[1].format == eStatFormat::TimeElapsedRange && tt[1].labelId == 0x5bd);

    // --- Rampages screen: 37 rows; two headers, score rows are placeholders with real medals ---
    save.ambientMedal[0x22] = 1;      // first rampage medal (bronze)
    auto ramp = PdaStats_BuildRampages(s, save);
    assert(ramp.size() == 37);
    assert(ramp[0].format == eStatFormat::Identity);
    assert(ramp[1].available == false && ramp[1].labelId == 0x618 && ramp[1].sprite == 1);
    assert(ramp[31].format == eStatFormat::Identity);                    // turret-rampage header

    // --- ShootingRange screen: 5 rows, all placeholder scores with real medals ---
    save.ambientMedal[0x1a] = 3;
    auto shoot = PdaStats_BuildShootingRange(s, save);
    assert(shoot.size() == 5);
    assert(shoot[0].labelId == 0x609 && shoot[0].available == false && shoot[0].sprite == 3);
    assert(shoot.back().labelId == 0x60d);

    // --- Medal sprite id mapping (cPauseStatsApp::AddStatToListBox 0x845fc4): kind 1->0x52, 2->0x54, other->0x53 ---
    assert(PdaStatsApp::medalSpriteId(0) == 0);          // kind 0 = no medal
    assert(PdaStatsApp::medalSpriteId(1) == 0x52);
    assert(PdaStatsApp::medalSpriteId(2) == 0x54);
    assert(PdaStatsApp::medalSpriteId(3) == 0x53);       // any other non-zero kind
    assert(PdaStatsApp::medalSpriteId(255) == 0x53);

    // --- Dispatch + title reachability for every category ---
    for (int c = 0; c < (int)StatsCategory::Count; ++c)
        assert(!PdaStats_Build((StatsCategory)c, s, save).empty());      // every category builds rows
    assert(PdaStats_TitleId(StatsCategory::Races) == 0x53b);

    // --- App navigation ---
    PdaStatsApp app;
    app.init(&s, &save, {});
    assert(app.category() == StatsCategory::General);
    assert(app.rows().size() == 21);
    assert(app.input(PdaStatsApp::Input::NextCategory));
    assert(app.category() == StatsCategory::Crime);
    assert(app.rows().size() == 7);
    // Paging forward must reach the last (sub-screen) category, ShootingRange, through the full range.
    while (app.input(PdaStatsApp::Input::NextCategory)) {}
    assert(app.category() == StatsCategory::ShootingRange);
    assert(app.rows().size() == 5);
    assert(app.input(PdaStatsApp::Input::PrevCategory));
    assert(app.category() == StatsCategory::Rampages);
    assert(app.rows().size() == 37);

    std::printf("pdastats_test: all assertions passed\n");
    return 0;
}
