// Tests for the drug-dealing economy (port/game/dealer.*): buy/sell money and
// inventory, the per-tier price bands and random-walk fluctuation, the global
// stash cap, standard-price reset and the buy-in bust odds. Synthetic data only;
// it exercises DrugMarket in isolation through a dedicated friend-access struct.

#include "dealer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace ctw {

// Friend access to DrugMarket internals so the test can read the cost basis and
// drive the fluctuating standard price without going through game systems.
struct DrugMarketTestAccess {
    static void setStandardPrice(DrugMarket& m, int idx, int price) {
        m.market_[idx].standardPrice = price;
    }
    static uint32_t totalPaid(const DrugMarket& m, int idx) {
        return m.inventory_[idx].totalPaid;
    }
};

} // namespace ctw

using namespace ctw;

namespace {

// A fake wallet mirroring cPlayer money clamps: take floors at 0, give caps at
// 99,999,999. The economy under test guards affordability before any debit.
struct FakeWallet : IWallet {
    int32_t cash;
    explicit FakeWallet(int32_t c) : cash(c) {}
    int32_t money() const override { return cash; }
    void giveMoney(uint32_t amount) override {
        int64_t v = static_cast<int64_t>(cash) + amount;
        if (v > 99999999) v = 99999999;
        cash = static_cast<int32_t>(v);
    }
    void takeMoney(uint32_t amount) override {
        int64_t v = static_cast<int64_t>(cash) - static_cast<int64_t>(amount);
        cash = v < 0 ? 0 : static_cast<int32_t>(v);
    }
};

// Scripted RandomInt: the market tick gate is RandomInt(0,10); everything else
// is a price delta. gate controls whether a commodity ticks, delta the amount.
struct Rng {
    int gate;
    int delta;
    RandomIntFn fn() {
        return [this](int lo, int hi) {
            if (lo == 0 && hi == 10) return gate;
            return delta;
        };
    }
};

int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++failures; }
}

void testBuyDeductsMoneyAndAddsInventory() {
    DrugMarket m;
    int idx = m.addCommodity(0x448, DrugTier::Cheap, 50);
    check(idx == 0, "first commodity index 0");
    FakeWallet w(1000);
    bool ok = m.buy(w, idx, 5, 50);           // cost 5 * 50 = 250
    check(ok, "buy succeeds with funds");
    check(w.cash == 750, "money debited by cost (1000-250)");
    check(m.quantityOf(idx) == 5, "inventory holds 5 units");
    check(m.heldTotal() == 5, "held total is 5");
    check(DrugMarketTestAccess::totalPaid(m, idx) == 250, "cost basis is 250");
}

void testBuyRejectedWhenBroke() {
    DrugMarket m;
    int idx = m.addCommodity(0x448, DrugTier::Cheap, 50);
    FakeWallet w(100);
    bool ok = m.buy(w, idx, 5, 50);           // cost 250 > 100
    check(!ok, "buy fails without funds");
    check(w.cash == 100, "money untouched on failed buy");
    check(m.quantityOf(idx) == 0, "no inventory on failed buy");
}

void testStashCapEnforced() {
    DrugMarket m;
    int idx = m.addCommodity(0x448, DrugTier::Cheap, 1);
    FakeWallet w(1000000);
    check(m.buy(w, idx, 198, 1), "buy 198 units ok");
    check(m.heldTotal() == 198, "held total 198");
    check(!m.buy(w, idx, 5, 1), "buy that overflows 200 cap fails");
    check(m.heldTotal() == 198, "held total unchanged after failed buy");
    check(m.buy(w, idx, 2, 1), "buy exactly to 200 ok");
    check(m.heldTotal() == 200, "held total at cap 200");
    check(!m.buy(w, idx, 1, 1), "buy past cap fails");
}

void testSellCreditsMoneyAndReducesInventory() {
    DrugMarket m;
    int idx = m.addCommodity(0x448, DrugTier::Cheap, 50);
    FakeWallet w(1000);
    m.buy(w, idx, 5, 50);                      // money 750, basis 250
    uint16_t sold = m.sell(w, idx, 3, 80);     // credit 3 * 80 = 240
    check(sold == 3, "sold 3 units");
    check(w.cash == 990, "money credited by proceeds (750+240)");
    check(m.quantityOf(idx) == 2, "2 units remain");
    check(m.heldTotal() == 2, "held total 2");
    // Cost basis reduced in proportion: 250 * 3/5 = 150 removed, 100 remains.
    check(DrugMarketTestAccess::totalPaid(m, idx) == 100, "cost basis prorated to 100");
}

void testSellClampsToHeld() {
    DrugMarket m;
    int idx = m.addCommodity(0x448, DrugTier::Cheap, 50);
    FakeWallet w(1000);
    m.buy(w, idx, 2, 50);                       // money 900, 2 held
    uint16_t sold = m.sell(w, idx, 5, 100);     // only 2 to sell -> credit 200
    check(sold == 2, "sell clamps to held quantity");
    check(w.cash == 1100, "credited for the 2 sold (900+200)");
    check(m.quantityOf(idx) == 0, "inventory emptied");
    check(m.heldTotal() == 0, "held total 0");
    check(DrugMarketTestAccess::totalPaid(m, idx) == 0, "cost basis cleared");
}

void testPriceFluctuationWithinBand() {
    // In-band move accepted.
    {
        DrugMarket m;
        m.addCommodity(0x448, DrugTier::Cheap, 50);
        Rng rng{0, 2};                          // tick, delta +2
        m.updatePrices(rng.fn());
        check(m.commodity(0).standardPrice == 52, "cheap price +2 -> 52 (in band)");
    }
    // Move that would exceed the band is rejected (price kept).
    {
        DrugMarket m;
        m.addCommodity(0x448, DrugTier::Cheap, 100);   // at the 100 ceiling
        Rng rng{0, 2};                          // tick, delta +2 -> 102 > 100
        m.updatePrices(rng.fn());
        check(m.commodity(0).standardPrice == 100, "cheap price capped at 100 (reject)");
    }
    // No tick when the gate roll is nonzero.
    {
        DrugMarket m;
        m.addCommodity(0x448, DrugTier::Cheap, 50);
        Rng rng{1, 2};                          // gate != 0 -> skip
        m.updatePrices(rng.fn());
        check(m.commodity(0).standardPrice == 50, "no tick when gate nonzero");
    }
    // Mid-tier lower-band reject and accept.
    {
        DrugMarket m;
        m.addCommodity(0x44a, DrugTier::Mid, 152);
        Rng rng{0, -5};                         // 152-5 = 147 < 150 -> reject
        m.updatePrices(rng.fn());
        check(m.commodity(0).standardPrice == 152, "mid price floor at 150 (reject)");
        DrugMarketTestAccess::setStandardPrice(m, 0, 160);
        m.updatePrices(rng.fn());               // 160-5 = 155 in [150,500] -> accept
        check(m.commodity(0).standardPrice == 155, "mid price -5 -> 155 (in band)");
    }
}

void testResetStandardPrices() {
    DrugMarket m;
    int a = m.addCommodity(0x448, DrugTier::Cheap, 50);   // base 50 (in band)
    int b = m.addCommodity(0x44a, DrugTier::Cheap, 5);    // base 5 (below the 8 floor)
    DrugMarketTestAccess::setStandardPrice(m, a, 80);
    DrugMarketTestAccess::setStandardPrice(m, b, 50);
    m.resetStandardPrices();
    check(m.commodity(a).standardPrice == 50, "reset restores in-band base");
    check(m.commodity(b).standardPrice == 50, "reset keeps price when base out of band");
}

void testBustUpperBound() {
    check(DrugMarket::bustUpperBound(0) == -1, "no bust roll for zero value");
    check(DrugMarket::bustUpperBound(50) == 50, "value<100 -> 50");
    check(DrugMarket::bustUpperBound(99) == 50, "value 99 -> 50");
    check(DrugMarket::bustUpperBound(100) == 25, "value 100 -> 25");
    check(DrugMarket::bustUpperBound(499) == 25, "value 499 -> 25");
    check(DrugMarket::bustUpperBound(500) == 3, "value 500 -> 3");
    check(DrugMarket::bustUpperBound(5000) == 3, "high value -> 3");
}

void testMarketCapacity() {
    DrugMarket m;
    for (int i = 0; i < DrugMarket::kMaxCommodities; ++i)
        check(m.addCommodity(0x448, DrugTier::Cheap, 50) == i, "commodity added");
    check(m.addCommodity(0x448, DrugTier::Cheap, 50) == -1, "market full at 11");
}

} // namespace

int main() {
    testBuyDeductsMoneyAndAddsInventory();
    testBuyRejectedWhenBroke();
    testStashCapEnforced();
    testSellCreditsMoneyAndReducesInventory();
    testSellClampsToHeld();
    testPriceFluctuationWithinBand();
    testResetStandardPrices();
    testBustUpperBound();
    testMarketCapacity();
    if (failures == 0)
        std::printf("dealer_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
