// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
//
// Shops: the in-game store where the player buys weapons, ammo and body armour. In the original this is the
// "Ammu-Zone" PDA app (Gui::cAmmozoneApp) backed by SaveGameImpl's Ammozone data, with the item economics in
// SaveGameImpl::GetAmmozoneItemCost. This port keeps the *economics* (stock, pricing, affordability, basket and
// the purchase charge) as a self-contained component, decoupled from the PDA UI and from the save game so it can
// be driven from the dev menu or a test. The weapon/item names are GXT strings looked up by id at run time
// (cWeaponInfo +0x28, e_main.gxt via Weapons_Name); no names or price tables are baked in here.
//
// Verified against libGame.so (Android ARM64):
//   SaveGameImpl::GetAmmozoneItemCost        0x008bc44c  price = base*(100-discount)/(onOffer?200:100)
//   cAmmozoneApp::CanAddWeaponToBasket       0x007492e4  availability + affordability + ammo-cap gates
//   cAmmozoneApp::AddWeaponToBasket          0x007496f0  gate, then moneyAvailable -= cost, order qty++
//   cAmmozoneApp::ChangeMoneyAvailable       0x00749920  moneyAvailable += delta
//   cAmmozoneApp::DeleteAllFromBasket        0x0074a108  clears the basket; moneyAvailable back to player money
//   cAmmozoneApp::MakePurchase               0x0074a18c  charge = playerMoney - moneyAvailable; TakeMoney; stat
//   SaveGameImpl::AmmozoneGetDiscount/Set    0x008bdcdc / 0x008bb770  the global 0..100 discount percent
//
// The original does NOT hand the weapons over at purchase time: the order is stored and later delivered as pickups
// by cAmmoZoneTruck (SpawnGoodies 0x00cf463c). deliverOrder() here stands in for that delivery and reuses the
// port's weapon-give (Player::giveWeapon / WeaponManager::give); see shops.cpp.
#pragma once
#include <cstdint>
#include <vector>

class Game;

namespace shop {

// One line of a shop's stock. Mirrors sAmmozoneData: a u16 base price and the weapon type (name is a GXT id
// resolved from the weapon type at run time, never stored here). "available" and "onOffer" are the per-item flags
// the original packs into the low two bits of the type byte (value 2 == on offer -> half price).
struct ShopItem {
    int weaponType = 0;        // eWeaponType; its GXT name = Weapons_Name(weaponType)
    uint16_t basePrice = 0;    // sAmmozoneData +0, the undiscounted price
    bool available = true;     // PDAImpl::AmmozoneSetWeaponAvailable
    bool onOffer = false;      // PDAImpl::AmmozoneSetWeaponOnOffer -> half price
};

// Why an item cannot be added to the basket (a reduced, behaviour-faithful subset of eCantBuyReason).
enum class CantBuy {
    Ok = 0,
    Unavailable,   // the item is not currently stocked / enabled
    CantAfford,    // its discounted cost exceeds the money still available
    AmmoFull,      // the player already holds (or has ordered) the weapon's maximum ammo
};

// A shop definition: its stock and the global discount. The pricing is pure and matches GetAmmozoneItemCost.
class Shop {
public:
    void clear() { stock_.clear(); }
    void addItem(const ShopItem& item) { stock_.push_back(item); }
    const std::vector<ShopItem>& stock() const { return stock_; }
    int size() const { return static_cast<int>(stock_.size()); }
    const ShopItem* item(int index) const;
    const ShopItem* find(int weaponType) const;

    // SaveGameImpl::AmmozoneGetDiscount / AmmozoneSetDiscount: a percent in 0..100.
    void setDiscount(uint32_t percent);
    uint32_t discount() const { return discount_; }
    void setAvailable(int weaponType, bool available);   // PDAImpl::AmmozoneSetWeaponAvailable
    void setOnOffer(int weaponType, bool onOffer);       // PDAImpl::AmmozoneSetWeaponOnOffer

    // SaveGameImpl::GetAmmozoneItemCost: base * (100 - discount) / (onOffer ? 200 : 100), or the raw base price
    // when the discount is not applied. Integer division, truncating, exactly as the binary.
    uint32_t itemCost(const ShopItem& item, bool applyDiscount = true) const;

private:
    std::vector<ShopItem> stock_;
    uint32_t discount_ = 0;   // 0..100
};

// One ordered line: a weapon type and how many "lots" of it are in the basket.
struct Order {
    int weaponType = 0;
    int quantity = 0;    // number of purchases; each grants Weapons_DefaultAmmo(weaponType) ammo
};

// How much ammo the player is treated as already holding for a weapon, so the ammo-cap gate can run without the
// engine present (in game this is the amount in the player's weapon slot). maxAmmo < 0 means infinite (melee).
struct HeldAmmo {
    int weaponType = 0;
    int ammo = 0;
    int maxAmmo = -1;    // cWeaponInfo +0x04
    int unitAmmo = 1;    // cWeaponManager::GetDefaultWeaponAmmo, ammo granted per purchase
};

// A shopping session over a Shop: the basket (cAmmozoneApp). open() snapshots the player's money; add/remove move
// the running "money available" (field 0xb80); total() is what MakePurchase charges.
class ShopSession {
public:
    ShopSession(const Shop& shop, int32_t playerMoney) { open(shop, playerMoney); }
    void open(const Shop& shop, int32_t playerMoney);

    // Supply (optional) the ammo the player already holds for a weapon, for the ammo-cap gate.
    void setHeld(const HeldAmmo& held);

    CantBuy canAdd(const ShopItem& item) const;    // cAmmozoneApp::CanAddWeaponToBasket (core gates)
    bool add(const ShopItem& item);                // cAmmozoneApp::AddWeaponToBasket
    bool remove(const ShopItem& item);             // the basket's remove-one (inverse of add)
    void clearBasket();                            // cAmmozoneApp::DeleteAllFromBasket

    int32_t moneyAvailable() const { return moneyAvailable_; }   // field 0xb80
    int32_t total() const { return playerMoney_ - moneyAvailable_; }   // the MakePurchase charge
    bool empty() const { return orders_.empty(); }
    const std::vector<Order>& orders() const { return orders_; }
    int orderedQuantity(int weaponType) const;

private:
    const Shop* shop_ = nullptr;
    int32_t playerMoney_ = 0;
    int32_t moneyAvailable_ = 0;
    std::vector<Order> orders_;
    std::vector<HeldAmmo> held_;
    void changeMoneyAvailable(int32_t delta) { moneyAvailable_ += delta; }   // cAmmozoneApp::ChangeMoneyAvailable
    const HeldAmmo* heldFor(int weaponType) const;
};

// cAmmozoneApp::MakePurchase: deduct the basket total from the player and record it on the weapons-spending stat.
// Returns the amount charged. Does NOT give the weapons (see deliverOrder). Reuses Game::takeMoney and
// Stats::addSpentOnWeapons.
uint32_t makePurchase(ShopSession& session, Game& game);

// The delivery step the original defers to cAmmoZoneTruck: grant every ordered weapon and its ammo to the player.
// Reuses Game::giveWeapon (quantity * Weapons_DefaultAmmo per line). Returns the number of weapon lots delivered.
int deliverOrder(const ShopSession& session, Game& game);

}  // namespace shop
