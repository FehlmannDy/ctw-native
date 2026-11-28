// The drug-dealing economy. See dealer.h for the data model and the binary
// addresses each routine is matched against. Constants are cited inline; no
// drug name (a GXT text id) and no price table are baked in.

#include "dealer.h"

namespace ctw {

DrugMarket::DrugMarket() {
    // Verified tier bands and random-walk deltas (cStockMarket::Update 0x00cf009c
    // accept tests + delta table 0x004ae1f0). {priceMin, priceMax, deltaMin, deltaMax}
    rules_[static_cast<int>(DrugTier::Cheap)]     = {   8,  100, -2,  2 };
    rules_[static_cast<int>(DrugTier::Mid)]       = { 150,  500, -5, 15 };
    rules_[static_cast<int>(DrugTier::Expensive)] = { 600, 1000, -1, 25 };
}

// cStockMarket::AddCommodityItems (0x00cefe64)
int DrugMarket::addCommodity(uint16_t textTagId, DrugTier tier, int standardPrice) {
    if (static_cast<int>(market_.size()) >= kMaxCommodities)
        return -1;
    Commodity c;
    c.textTagId = textTagId;
    c.tier = tier;
    c.standardPrice = standardPrice;
    c.basePrice = standardPrice;   // the reset target backed up at +0x160
    market_.push_back(c);
    return static_cast<int>(market_.size()) - 1;
}

// cStockMarket::Update (0x00cf009c) -- one price-fluctuation pass.
void DrugMarket::updatePrices(const RandomIntFn& rnd) {
    for (Commodity& c : market_) {
        // 0xcf0328: RandomInt(0,10); update this commodity only when the roll is 0.
        if (rnd(0, 10) != 0)
            continue;
        const TierRule& r = rule(c.tier);
        int delta = rnd(r.deltaMin, r.deltaMax);        // 0xcf0350: RandomInt(lo,hi)
        int candidate = c.standardPrice + delta;        // 0xcf0368: add w2, price, delta
        if (inBand(c.tier, candidate))                  // accept only if within the band
            c.standardPrice = candidate;
        // else: reject, keep the old price (0xcf0314 path)
    }
}

// cStockMarket::FinishedDeal (0x00923854) standard-price reset loop (0x92391c).
void DrugMarket::resetStandardPrices() {
    for (Commodity& c : market_) {
        if (inBand(c.tier, c.basePrice))
            c.standardPrice = c.basePrice;
        // else: leave the current standard price unchanged
    }
}

// cTradeManager::DoTrade buy branch (0x008c3888) + cInventory::AddCommodity (0x00877f30)
bool DrugMarket::buy(IWallet& wallet, int idx, uint16_t quantity, int unitPrice) {
    if (idx < 0 || idx >= static_cast<int>(market_.size()))
        return false;
    if (quantity == 0 || unitPrice < 0)
        return false;
    if (idx >= kMaxCommodities)            // slot id cap (0x00877f34: id <= 10)
        return false;
    // Global unit cap (0x00877f4c: held + qty <= 200).
    if (static_cast<int>(heldTotal_) + static_cast<int>(quantity) > kInventoryUnitCap)
        return false;
    uint32_t cost = static_cast<uint32_t>(unitPrice) * static_cast<uint32_t>(quantity);
    if (static_cast<int64_t>(cost) > static_cast<int64_t>(wallet.money()))
        return false;                       // can't afford it: nothing moves

    wallet.takeMoney(cost);
    inventory_[idx].quantity = static_cast<uint16_t>(inventory_[idx].quantity + quantity);
    inventory_[idx].totalPaid += cost;      // accumulated cost basis (+4 in the slot)
    heldTotal_ = static_cast<uint16_t>(heldTotal_ + quantity);
    return true;
}

// cTradeManager::DoTrade sell branch (0x008c3908)
uint16_t DrugMarket::sell(IWallet& wallet, int idx, uint16_t quantity, int unitPrice) {
    if (idx < 0 || idx >= static_cast<int>(market_.size()) || idx >= kMaxCommodities)
        return 0;
    if (quantity == 0 || unitPrice < 0)
        return 0;
    // 0x008c3860: subs qty, held, amount; csel ...,wzr,hi -> sell at most what is held.
    uint16_t held = inventory_[idx].quantity;
    uint16_t sold = quantity <= held ? quantity : held;
    if (sold == 0)
        return 0;

    uint32_t credit = static_cast<uint32_t>(unitPrice) * static_cast<uint32_t>(sold);
    // Reduce the cost basis in proportion to the fraction of the slot sold.
    uint32_t paidBack = inventory_[idx].totalPaid;
    if (held > 0 && sold < held)
        paidBack = static_cast<uint32_t>(
            (static_cast<uint64_t>(inventory_[idx].totalPaid) * sold) / held);

    inventory_[idx].quantity = static_cast<uint16_t>(held - sold);
    inventory_[idx].totalPaid -= paidBack;
    heldTotal_ = static_cast<uint16_t>(heldTotal_ - sold);
    wallet.giveMoney(credit);
    return sold;
}

uint16_t DrugMarket::quantityOf(int idx) const {
    if (idx < 0 || idx >= kMaxCommodities)
        return 0;
    return inventory_[idx].quantity;
}

// cStockMarket::FinishedDeal buy-in odds (0x009239b0).
int DrugMarket::bustUpperBound(int dealValue) {
    if (dealValue < 1)
        return -1;                 // no deal value -> no bust roll
    if (dealValue < 100)           // 0x64
        return 50;                 // 0x32  -> 1-in-51
    if (dealValue < 500)           // 0x1f4
        return 25;                 // 0x19  -> 1-in-26
    return 3;                      //        -> 1-in-4
}

} // namespace ctw
