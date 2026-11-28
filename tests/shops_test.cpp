// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
// The weapon-store economics (shop::Shop / shop::ShopSession): pricing with discount and offers, the affordability
// and ammo-cap gates, money-available bookkeeping and the purchase charge. Synthetic stock only (no GXT names, no
// price tables): each item is an invented weapon type and price.
#include "shops.h"
#include <cstdio>

using namespace shop;

static int failures;
static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

int main() {
    // Synthetic stock: type 5 @ $400, type 9 @ $1000, type 10 @ $1000 (on offer => half).
    Shop shop;
    shop.addItem(ShopItem{/*type*/5,  /*price*/400,  /*avail*/true,  /*offer*/false});
    shop.addItem(ShopItem{/*type*/9,  /*price*/1000, /*avail*/true,  /*offer*/false});
    shop.addItem(ShopItem{/*type*/10, /*price*/1000, /*avail*/true,  /*offer*/true});

    // GetAmmozoneItemCost: base * (100 - discount) / (onOffer ? 200 : 100).
    check(shop.itemCost(*shop.find(5)) == 400, "no discount, no offer: full price");
    check(shop.itemCost(*shop.find(10)) == 500, "on offer halves the price");
    check(shop.itemCost(*shop.find(5), /*applyDiscount*/false) == 400, "raw price ignores discount flag");
    shop.setDiscount(25);
    check(shop.itemCost(*shop.find(5)) == 300, "25% off $400 is $300");
    check(shop.itemCost(*shop.find(10)) == 375, "25% off then halved: 1000*75/200 = 375");
    shop.setDiscount(150);
    check(shop.discount() == 100, "discount clamps to 100 (stored as a byte)");
    check(shop.itemCost(*shop.find(5)) == 0, "a 100% discount makes it free");
    shop.setDiscount(0);

    // Affordability and money deducted.
    {
        ShopSession s(shop, /*playerMoney*/1500);
        check(s.canAdd(*shop.find(9)) == CantBuy::Ok, "can afford a $1000 item with $1500");
        check(s.add(*shop.find(9)), "add the $1000 item");
        check(s.moneyAvailable() == 500 && s.total() == 1000, "money available drops by the cost; total is the charge");
        check(s.canAdd(*shop.find(9)) == CantBuy::CantAfford, "cannot afford a second $1000 item with $500 left");
        check(!s.add(*shop.find(9)), "the add is rejected");
        check(s.canAdd(*shop.find(5)) == CantBuy::Ok && s.add(*shop.find(5)), "but a $400 item still fits");
        check(s.moneyAvailable() == 100 && s.total() == 1400, "two items charge $1400");
        check(s.remove(*shop.find(5)), "remove the $400 item");
        check(s.moneyAvailable() == 500 && s.total() == 1000, "removing restores its cost");
        s.clearBasket();
        check(s.empty() && s.moneyAvailable() == 1500 && s.total() == 0, "clearing empties the basket");
    }

    // Availability gate.
    {
        shop.setAvailable(9, false);
        ShopSession s(shop, 100000);
        check(s.canAdd(*shop.find(9)) == CantBuy::Unavailable, "an unavailable item cannot be bought");
        shop.setAvailable(9, true);
    }

    // Ammo-cap gate and weapon/ammo granted. Held 40/90 of type 9, 50 ammo per purchase => one lot fits, not two.
    {
        ShopSession s(shop, 100000);
        s.setHeld(HeldAmmo{/*type*/9, /*ammo*/40, /*maxAmmo*/90, /*unit*/50});
        check(s.canAdd(*shop.find(9)) == CantBuy::Ok, "one lot (50) still fits under the 90 cap from 40");
        check(s.add(*shop.find(9)), "first lot added");
        check(s.canAdd(*shop.find(9)) == CantBuy::AmmoFull, "a second lot would exceed the ammo cap");
        check(s.orderedQuantity(9) == 1, "exactly one lot ordered");
        // What delivery would grant: quantity * unit ammo = 1 * 50.
        int granted = s.orderedQuantity(9) * 50;
        check(granted == 50, "delivery grants quantity * per-purchase ammo");
    }

    // Melee (infinite ammo, maxAmmo < 0) is never ammo-capped.
    {
        Shop m;
        m.addItem(ShopItem{/*type*/3, /*price*/0, true, false});   // a sword, free
        ShopSession s(m, 10);
        s.setHeld(HeldAmmo{/*type*/3, /*ammo*/1, /*maxAmmo*/-1, /*unit*/1});
        check(s.canAdd(*m.find(3)) == CantBuy::Ok && s.add(*m.find(3)), "a melee weapon is not ammo-capped");
        check(s.add(*m.find(3)), "and can be added again");
    }

    if (failures == 0) std::printf("shops_test: all checks passed\n");
    return failures ? 1 : 0;
}
