// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// cEmptyTheBootApp state machine, checked against libGame.so with synthetic input. Every asserted number is
// re-derived from the binary constants documented in emptytheboot.h:
//   6 papers (+0x1fc from 0x47af68); open gate DragMagnitude>=0x29, DirY>=1, angle<=0x1ff (Process state 0);
//   scrape gate travel>15 px, DirX>=1 (Process state 4); fire 6 stages x 31 frames (UpdateFire 0x7fd998);
//   sfx 0x27b (open), 0x2e6/0x7f (scrape), 0x1ed/0x25a loops (ProcessSfxLoops); IsFinished is a stub returning 0.
// No GL/assets/audio: the one-shot sfx go through the playSfx hook, the loops through the loop-playing flags.
#include "emptytheboot.h"
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

using etb::EmptyTheBoot;
using etb::State;

static int failures;
static void check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; } }

// Advance Process n times.
static void tick(EmptyTheBoot& g, int n) { for (int i = 0; i < n; ++i) g.process(); }

// Open the boot, settle, and empty all 6 papers; leaves the game in MatchSetup (then one tick -> MatchScrape).
static void openAndEmpty(EmptyTheBoot& g) {
    g.swipeOpen(EmptyTheBoot::kOpenDragMagMin, 1, EmptyTheBoot::kOpenAngleMax);  // -> Opening
    tick(g, 2);                                                                  // Opening settle -> Papers
    for (int i = 0; i < EmptyTheBoot::kPapers; ++i) g.dropPaper();               // -> MatchSetup on the 6th
}

int main() {
    // --- the open gate: magnitude, direction and angle must all pass -----------------------------------------
    {
        auto gp = std::make_unique<EmptyTheBoot>();
        EmptyTheBoot& g = *gp;
        std::vector<std::pair<int,int>> sfx;
        g.playSfx = [&](int s, int v) { sfx.push_back({s, v}); };
        g.start();
        check(g.state() == State::Closed, "starts Closed");
        check(g.papersLeft() == 6, "starts with 6 papers");
        check(!g.swipeOpen(EmptyTheBoot::kOpenDragMagMin - 1, 1, 0), "too-weak swipe (mag 40) does not open");
        check(!g.swipeOpen(EmptyTheBoot::kOpenDragMagMin, 0, 0), "swipe with DirY 0 does not open");
        check(!g.swipeOpen(EmptyTheBoot::kOpenDragMagMin, 1, EmptyTheBoot::kOpenAngleMax + 1),
              "swipe past the angle limit (0x200) does not open");
        check(g.state() == State::Closed, "failed swipes leave it Closed");
        check(sfx.empty(), "no sfx on a failed swipe");
        check(g.swipeOpen(EmptyTheBoot::kOpenDragMagMin, 1, EmptyTheBoot::kOpenAngleMax),
              "swipe at exactly the thresholds opens the boot");
        check(g.state() == State::Opening, "opening the boot enters Opening");
        check(sfx.size() == 1 && sfx[0].first == EmptyTheBoot::kSfxOpenBoot && sfx[0].second == 0x64,
              "OpenBoot plays sfx 0x27b at vol 0x64");
    }

    // --- Opening settles to Papers after two frames; dropping 6 papers reaches the match -----------------------
    {
        auto gp = std::make_unique<EmptyTheBoot>();
        EmptyTheBoot& g = *gp;
        g.start();
        g.swipeOpen(EmptyTheBoot::kOpenDragMagMin, 1, 0);
        check(g.state() == State::Opening, "in Opening before settling");
        g.process();
        check(g.state() == State::Opening, "still Opening after one frame (counter == 1)");
        g.process();
        check(g.state() == State::Papers, "Opening settles to Papers on the second frame");

        check(!g.scrapeMatch(100, 1), "cannot scrape the match while still on the papers");
        for (int i = 6; i >= 1; --i) {
            check(g.papersLeft() == i, "paper count before a drop");
            const bool removed = g.dropPaper();
            check(removed, "dropping a paper on the target removes it");
            check(g.papersLeft() == i - 1, "each drop decrements the paper count by one");
            if (i > 1) check(g.state() == State::Papers, "still emptying while papers remain");
        }
        check(g.papersLeft() == 0, "all 6 papers removed");
        check(g.state() == State::MatchSetup, "emptying the last paper shows the matches (MatchSetup)");
        check(!g.dropPaper(), "no papers left to drop");
    }

    // --- the match: setup settles to scrape; the scrape gate; lighting advances to the fire -------------------
    {
        auto gp = std::make_unique<EmptyTheBoot>();
        EmptyTheBoot& g = *gp;
        std::vector<std::pair<int,int>> sfx;
        g.playSfx = [&](int s, int v) { sfx.push_back({s, v}); };
        g.start();
        openAndEmpty(g);
        check(g.state() == State::MatchSetup, "at MatchSetup after emptying the boot");
        g.process();
        check(g.state() == State::MatchScrape, "MatchSetup settles to MatchScrape in one frame");
        sfx.clear();   // drop the OpenBoot sfx that openAndEmpty's successful swipe recorded, so the scrape asserts are clean

        check(!g.scrapeMatch(EmptyTheBoot::kScrapeDistMin, 1), "a 15 px scrape is not enough to light the match");
        check(!g.matchLit(), "match not lit by a too-short scrape");
        check(!g.scrapeMatch(EmptyTheBoot::kScrapeDistMin + 1, 0), "a scrape with DirX 0 does not light the match");
        check(!g.matchLit(), "match still unlit");
        check(sfx.empty(), "no scrape sfx on a failed scrape");

        check(g.scrapeMatch(EmptyTheBoot::kScrapeDistMin + 1, 1), "a 16 px scrape with DirX>=1 lights the match");
        check(g.matchLit(), "match is now lit");
        check(sfx.size() == 1 && sfx[0].first == EmptyTheBoot::kSfxScrape && sfx[0].second == 0x7f,
              "a scrape plays sfx 0x2e6 at vol 0x7f");
        check(!g.scrapeMatch(100, 1), "scraping an already-lit match does not re-light (returns false)");

        check(!g.matchLoopPlaying(), "match-burn loop not flagged until Process runs ProcessSfxLoops");
        g.process();   // MatchScrape -> MatchThrow, ProcessSfxLoops turns the match loop on
        check(g.state() == State::MatchThrow, "a lit match advances to the throw");
        check(g.matchLoopPlaying(), "the match-burn loop (0x25a) runs while the match is lit");
        g.process();   // MatchThrow -> StartFire -> Fire
        check(g.state() == State::Fire, "the thrown match starts the fire");
        check(g.fireLoopPlaying(), "the fire crackle loop (0x1ed) runs while burning");
        check(g.fireStage() == 0, "fire starts at stage 0");
    }

    // --- the fire burns through 6 stages of 31 frames, then the boot is emptied (success) ---------------------
    {
        auto gp = std::make_unique<EmptyTheBoot>();
        EmptyTheBoot& g = *gp;
        bool doneCalled = false, doneSuccess = false;
        g.start([&](bool s) { doneCalled = true; doneSuccess = s; });
        openAndEmpty(g);
        tick(g, 1);    // -> MatchScrape
        g.scrapeMatch(16, 1);
        tick(g, 2);    // -> Fire (burn not yet stepped)
        check(g.state() == State::Fire && g.fireStage() == 0, "in Fire at stage 0 before the burn steps");

        // Stage advances on exactly the 31st frame of each stage.
        tick(g, EmptyTheBoot::kFireStageFrames - 1);     // 30 frames
        check(g.fireStage() == 0, "stage still 0 after 30 frames");
        g.process();                                      // the 31st frame
        check(g.fireStage() == 1, "stage advances to 1 on the 31st frame");

        // Drive the remaining stages. Total burn is 6 * 31 = 186 frames from entering Fire; we have used 31.
        tick(g, EmptyTheBoot::kFireStageFrames * (EmptyTheBoot::kFireStages - 1) - 1);  // up to the frame before done
        check(!g.finished(), "not finished until the burn fully grows");
        check(g.state() == State::Fire, "still in Fire just before completion");
        g.process();                                      // frame 186: stage reaches 6 -> done
        check(g.finished(), "the fire completing finishes the minigame");
        check(doneCalled && doneSuccess, "a completed burn reports success");
        check(!g.fireLoopPlaying() && !g.matchLoopPlaying(), "both sfx loops stop when the app finishes");
        check(!g.isFinished(), "IsFinished is the binary's stub: always false");
    }

    // --- the exit button gives up (failure), and is only a give-up, not a loss condition ----------------------
    {
        auto gp = std::make_unique<EmptyTheBoot>();
        EmptyTheBoot& g = *gp;
        bool doneCalled = false, doneSuccess = true;
        g.start([&](bool s) { doneCalled = true; doneSuccess = s; });
        g.swipeOpen(EmptyTheBoot::kOpenDragMagMin, 1, 0);
        tick(g, 2);
        g.dropPaper();                       // partway through the papers
        check(g.state() == State::Papers && g.papersLeft() == 5, "quitting mid-papers");
        g.quit();
        check(g.finished(), "the exit button finishes the minigame");
        check(doneCalled && !doneSuccess, "giving up reports failure (success == false)");
        g.process();                          // no-op once finished
        check(g.state() == State::Papers, "a finished game does not advance");
    }

    if (failures == 0) std::printf("emptytheboot: all checks passed\n");
    return failures ? 1 : 0;
}
