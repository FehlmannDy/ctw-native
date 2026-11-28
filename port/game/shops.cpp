// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// The weapon/armour store economics (the "Ammu-Zone" app). See shops.h for the map to the original routines and
// the verified addresses. Nothing here bakes an item name or a price table: stock is supplied at run time and the
// names are GXT strings resolved from the weapon type elsewhere (Weapons_Name).
#include "shops.h"

#include "game.h"
#include "weapons.h"

namespace shop {

const ShopItem* Shop::item(int index) const {
    if (index < 0 || index >= static_cast<int>(stock_.size())) return nullptr;
    return &stock_[index];
}

const ShopItem* Shop::find(int weaponType) const {
    for (const ShopItem& it : stock_)
        if (it.weaponType == weaponType) return &it;
    return nullptr;
}

void Shop::setDiscount(uint32_t percent) {   // SaveGameImpl::AmmozoneSetDiscount: stored as a byte, so 0..100
    discount_ = percent > 100 ? 100 : percent;
}

void Shop::setAvailable(int weaponType, bool available) {
    for (ShopItem& it : stock_)
        if (it.weaponType == weaponType) it.available = available;
}

void Shop::setOnOffer(int weaponType, bool onOffer) {
    for (ShopItem& it : stock_)
        if (it.weaponType == weaponType) it.onOffer = onOffer;
}

// SaveGameImpl::GetAmmozoneItemCost @0x008bc44c:
//   price = base * (100 - discount) / (onOffer ? 200 : 100)        [applyDiscount]
//   price = base                                                   [otherwise]
// The multiply then integer-divide (udiv) truncates, matching the binary exactly.
uint32_t Shop::itemCost(const ShopItem& item, bool applyDiscount) const {
    if (!applyDiscount) return item.basePrice;
    uint32_t disc = discount_ > 100 ? 100 : discount_;
    uint32_t numerator = (100u - disc) * static_cast<uint32_t>(item.basePrice);
    uint32_t divisor = item.onOffer ? 200u : 100u;
    return numerator / divisor;
}

void ShopSession::open(const Shop& shop, int32_t playerMoney) {
    shop_ = &shop;
    playerMoney_ = playerMoney;
    moneyAvailable_ = playerMoney;   // cAmmozoneApp::Init: the basket starts empty, all money available
    orders_.clear();
    held_.clear();
}

void ShopSession::setHeld(const HeldAmmo& held) {
    for (HeldAmmo& h : held_) {
        if (h.weaponType == held.weaponType) { h = held; return; }
    }
    held_.push_back(held);
}

const HeldAmmo* ShopSession::heldFor(int weaponType) const {
    for (const HeldAmmo& h : held_)
        if (h.weaponType == weaponType) return &h;
    return nullptr;
}

int ShopSession::orderedQuantity(int weaponType) const {
    for (const Order& o : orders_)
        if (o.weaponType == weaponType) return o.quantity;
    return 0;
}

// cAmmozoneApp::CanAddWeaponToBasket @0x007492e4 (the core gates):
//   - the item must be available;
//   - the ammo already held plus what is already in the basket must leave room for one more lot
//     ((maxAmmo - held) / unitAmmo gives the lots that still fit; infinite-ammo weapons are never capped);
//   - the discounted cost must not exceed the money still available (the affordability test, shared with
//     AddWeaponToBasket @0x749748: moneyAvailable < cost => cannot add).
CantBuy ShopSession::canAdd(const ShopItem& item) const {
    if (!shop_) return CantBuy::Unavailable;
    if (!item.available) return CantBuy::Unavailable;

    if (const HeldAmmo* h = heldFor(item.weaponType)) {
        if (h->maxAmmo >= 0) {                       // melee / taser keep maxAmmo < 0 and are never ammo-capped
            int unit = h->unitAmmo > 0 ? h->unitAmmo : 1;
            int lotsThatFit = (h->maxAmmo - h->ammo) / unit;
            if (lotsThatFit < 0) lotsThatFit = 0;
            if (orderedQuantity(item.weaponType) >= lotsThatFit) return CantBuy::AmmoFull;
        }
    }

    if (static_cast<int32_t>(shop_->itemCost(item)) > moneyAvailable_) return CantBuy::CantAfford;
    return CantBuy::Ok;
}

// cAmmozoneApp::AddWeaponToBasket @0x007496f0: gate on CanAddWeaponToBasket, then bump the order quantity and
// decrement the running money available by the discounted cost (via ChangeMoneyAvailable).
bool ShopSession::add(const ShopItem& item) {
    if (canAdd(item) != CantBuy::Ok) return false;
    uint32_t cost = shop_->itemCost(item);
    for (Order& o : orders_) {
        if (o.weaponType == item.weaponType) {
            ++o.quantity;
            changeMoneyAvailable(-static_cast<int32_t>(cost));
            return true;
        }
    }
    orders_.push_back(Order{item.weaponType, 1});
    changeMoneyAvailable(-static_cast<int32_t>(cost));
    return true;
}

// Remove one lot of an item from the basket (the inverse of add), restoring its cost to the money available.
bool ShopSession::remove(const ShopItem& item) {
    for (size_t i = 0; i < orders_.size(); ++i) {
        if (orders_[i].weaponType == item.weaponType && orders_[i].quantity > 0) {
            --orders_[i].quantity;
            changeMoneyAvailable(static_cast<int32_t>(shop_->itemCost(item)));
            if (orders_[i].quantity == 0) orders_.erase(orders_.begin() + i);
            return true;
        }
    }
    return false;
}

// cAmmozoneApp::DeleteAllFromBasket @0x0074a108: empty the basket; money available returns to the player's money.
void ShopSession::clearBasket() {
    orders_.clear();
    moneyAvailable_ = playerMoney_;
}

// cAmmozoneApp::MakePurchase @0x0074a18c: charge = playerMoney - moneyAvailable (the sum of the basket costs),
// take it from the player and add it to the weapons-spending stat. The weapons are delivered separately.
uint32_t makePurchase(ShopSession& session, Game& game) {
    int32_t charge = game.money - session.moneyAvailable();   // cPlayer::money (+0x112C) - moneyAvailable
    if (charge < 0) charge = 0;
    game.takeMoney(static_cast<uint32_t>(charge));             // cPlayer::TakeMoney
    game.stats.addSpentOnWeapons(static_cast<uint32_t>(charge));  // cStatsManager::AddSpentOnWeapons
    return static_cast<uint32_t>(charge);
}

// Delivery (what cAmmoZoneTruck does in the original, by spawning pickups the player collects): grant each ordered
// weapon with quantity * its default per-purchase ammo, reusing Game::giveWeapon.
int deliverOrder(const ShopSession& session, Game& game) {
    int delivered = 0;
    for (const Order& o : session.orders()) {
        if (o.quantity <= 0) continue;
        int unit = Weapons_Ok() ? Weapons_DefaultAmmo(o.weaponType) : 1;
        if (unit <= 0) unit = 1;
        game.giveWeapon(o.weaponType, unit * o.quantity, /*replace=*/false);   // cPlayer::GiveWeapon
        ++delivered;
    }
    return delivered;
}

}  // namespace shop
