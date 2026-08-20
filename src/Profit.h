// Profit - estimates what a single unit of an item is actually worth, and
// classifies how it can be sold.
#pragma once

#include <cstdint>

#include "Gw2Api.h"

// GW2's trading post keeps 15% of any TP sale (5% listing + 10% exchange) -
// only 85% of a sell/buy-order price is money you'd actually end up with.
constexpr int64_t TP_FEE_NUMERATOR = 85;
constexpr int64_t TP_FEE_DENOMINATOR = 100;

struct SellClassification {
    bool canSellToVendor = false;
    bool canSellOnTp = false;
    bool canNotBeSold = false;
};

SellClassification ClassifyItemSellMethod(const Gw2ItemInfo& info);

// Copper value of one unit.
//
// useInstantSellPrice = false (default): best-case value - the max of
// (vendor value, if sellable), (85% of the lowest TP sell listing), and
// (85% of the highest TP buy order). Takes the max of both TP prices - not
// just the buy-order price - because a thin/one-sided market can leave
// either one higher.
//
// useInstantSellPrice = true: what you'd actually walk away with selling
// right now - the max of (vendor value, if sellable) and (85% of the
// highest TP buy order) only. The sell-listing price is excluded because
// realizing it means waiting for a buyer at that price, not an instant
// sale.
//
// Either way, TP prices are ignored entirely (not just fee-adjusted) for a
// soulbound-on-acquire drop - see Gw2ItemInfo::soulboundOnAcquire.
int64_t EstimateItemUnitValueInCopper(const Gw2ItemInfo& info, bool useInstantSellPrice = false);
