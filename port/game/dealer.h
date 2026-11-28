#ifndef CTW_GAME_DEALER_H
#define CTW_GAME_DEALER_H

// The drug-dealing economy: the fluctuating drug market (cStockMarket) and the
// buy/sell transaction core of the dealer trade (cTradeManager::DoTrade) that
// moves the player's money and the commodity inventory (cInventory).
//
// Ported routine-by-routine against the original Android ARM64 libGame.so.
// Addresses and numeric constants are cited from that binary; no price table or
// drug name is baked in. A drug's name is a GXT text id (cCommodity text tag),
// injected by the caller at run time -- never transcribed here. The per-tier
// price bands and random-walk deltas below are the market *algorithm* constants
// read from the binary (cStockMarket::Update 0x00cf009c and the delta table at
// 0x004ae1f0); the standard/base prices themselves are injected per commodity.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ctw {

// RandomInt(lo, hi) as the game provides it (_Z9RandomIntii): a value in the
// inclusive range [lo, hi]. Injected so price moves are deterministic in tests.
using RandomIntFn = std::function<int(int lo, int hi)>;

// The player's wallet. The port binds this to Game::money / Game::giveMoney /
// Game::takeMoney (cPlayer::GiveMoney 0x..395d0 / cPlayer::TakeMoney 0x..38b70);
// tests supply a fake. The economy never duplicates the money storage.
struct IWallet {
    virtual ~IWallet() = default;
    virtual int32_t money() const = 0;
    virtual void giveMoney(uint32_t amount) = 0;   // sale proceeds (credit)
    virtual void takeMoney(uint32_t amount) = 0;    // purchase cost (debit)
};

// A commodity's price tier. cStockMarket stores a per-commodity "type" (0,1,2)
// that selects the price band the fluctuating standard price is kept within and
// the random-walk delta range applied each market tick.
enum class DrugTier : uint8_t { Cheap = 0, Mid = 1, Expensive = 2 };

// Verified band + per-tick delta for each tier.
//   Cheap     price [8,100],   delta [-2, 2]
//   Mid       price [150,500], delta [-5,15]
//   Expensive price [600,1000],delta [-1,25]
// Bands from cStockMarket::Update's accept test (e.g. tier 0: sub p,#0x65 /
// cmn #0x5d -> [101-93,101-1]); deltas from the table at 0x004ae1f0
// (hi = {2,15,25} @+0, lo = {-2,-5,-1} @+0xc), applied as RandomInt(lo,hi).
struct TierRule {
    int priceMin;
    int priceMax;
    int deltaMin;
    int deltaMax;
};

// One tradable drug on the market. textTagId is the GXT id of the drug's name,
// injected (never baked). standardPrice is the current fluctuating market price;
// basePrice is the value restored by cStockMarket::FinishedDeal's reset.
struct Commodity {
    uint16_t textTagId = 0;
    DrugTier tier = DrugTier::Cheap;
    int standardPrice = 0;
    int basePrice = 0;
};

// The player's held drugs. cInventory keeps, per commodity slot, the quantity
// held and the accumulated price paid (cost basis). AddCommodity (0x00877f30)
// caps a single slot id at 10 and the global unit count at 200 (0xc8).
struct InventorySlot {
    uint16_t quantity = 0;
    uint32_t totalPaid = 0;
};

class DrugMarket {
public:
    // cInventory::AddCommodity limits: ids 0..10, and the total units held
    // across all slots may not exceed 200.
    static constexpr int kMaxCommodities = 11;      // id <= 10 (0x00877f34)
    static constexpr int kInventoryUnitCap = 200;   // 0xc8 (0x00877f4c)

    DrugMarket();

    const TierRule& rule(DrugTier t) const { return rules_[static_cast<int>(t)]; }

    // cStockMarket::AddCommodityItems (0x00cefe64): register a drug with its GXT
    // name id, tier and current standard price (from PDAImpl::GetStandardPrice at
    // run time). basePrice is seeded to the standard price (the reset target).
    // Returns the slot index, or -1 if the market is full.
    int addCommodity(uint16_t textTagId, DrugTier tier, int standardPrice);

    int commodityCount() const { return static_cast<int>(market_.size()); }
    const Commodity& commodity(int idx) const { return market_[idx]; }

    // cStockMarket::Update (0x00cf009c) price fluctuation: run one market pass.
    // Each commodity has a 1-in-11 chance to tick (RandomInt(0,10)==0); on a tick
    // a delta RandomInt(deltaMin,deltaMax) is added and the new price is accepted
    // only if it stays within the tier band, otherwise the old price is kept.
    // The scheduling/gating of this pass (the cStockMarket script process,
    // WorldImpl::AreDealersUnlocked, mission checks) is NOT ported -- see header.
    void updatePrices(const RandomIntFn& rnd);

    // cStockMarket::FinishedDeal (0x00923854) price reset: after a deal, restore
    // each commodity's standard price to its basePrice if that base is within the
    // tier band (else leave it). Returns nothing; stats/trophy side effects and
    // the buy-in bust event are separate (see bustUpperBound / header).
    void resetStandardPrices();

    // cTradeManager::DoTrade buy branch (0x008c3888 TakeMoney) + cInventory::
    // AddCommodity. Cost = unitPrice * quantity. Fails (no money moved, no
    // inventory change) if idx invalid, quantity 0, the slot id is out of range,
    // the global unit cap would be exceeded, or the wallet can't afford the cost.
    bool buy(IWallet& wallet, int idx, uint16_t quantity, int unitPrice);

    // cTradeManager::DoTrade sell branch (0x008c3908 GiveMoney). Sells up to the
    // quantity held (the surplus is ignored, mirroring the clamped decrement at
    // 0x008c3860). Credits unitPrice * soldQuantity. Returns the quantity sold.
    uint16_t sell(IWallet& wallet, int idx, uint16_t quantity, int unitPrice);

    // Held quantity / total units, for the trade UI and tests.
    uint16_t quantityOf(int idx) const;
    uint16_t heldTotal() const { return heldTotal_; }

    // cStockMarket::FinishedDeal buy-in ("bust") odds (0x009239b0): given the
    // total standard value of the drugs sold in a deal, the inclusive upper bound
    // N for RandomInt(0,N); the bust fires when the roll is 0. Higher value drugs
    // raise the chance: value<100 -> 50, 100..499 -> 25, >=500 -> 3. The actual
    // wanted-level / HUD objective it triggers is a cross-file hook (not ported).
    static int bustUpperBound(int dealValue);

private:
    friend struct DrugMarketTestAccess;

    bool inBand(DrugTier t, int price) const {
        const TierRule& r = rule(t);
        return price >= r.priceMin && price <= r.priceMax;
    }

    TierRule rules_[3];
    std::vector<Commodity> market_;
    InventorySlot inventory_[kMaxCommodities];
    uint16_t heldTotal_ = 0;
};

} // namespace ctw

#endif // CTW_GAME_DEALER_H
