#include "Profit.h"

#include <algorithm>

SellClassification ClassifyItemSellMethod(const Gw2ItemInfo& info)
{
    SellClassification result;
    result.canSellToVendor = !info.noSell && info.vendorValue > 0;
    // A soulbound-on-acquire drop can't be listed even when commerce/prices
    // has a real price from other players' unbound copies of the same id -
    // see Gw2ItemInfo::soulboundOnAcquire.
    result.canSellOnTp = !info.soulboundOnAcquire
        && (info.tpSellPriceInCopper > 0 || info.tpBuyPriceInCopper > 0);
    result.canNotBeSold = !result.canSellToVendor && !result.canSellOnTp;
    return result;
}

int64_t EstimateItemUnitValueInCopper(const Gw2ItemInfo& info, bool useInstantSellPrice)
{
    const int64_t vendorProfit = (!info.noSell && info.vendorValue > 0) ? info.vendorValue : 0;
    if (info.soulboundOnAcquire) {
        // TP prices on a soulbound-on-acquire drop reflect a market this
        // specific item can never actually be listed on - not a discount
        // off a real number, so they're excluded entirely rather than
        // fee-adjusted.
        return vendorProfit;
    }
    const int64_t tpBuyProfit = info.tpBuyPriceInCopper > 0
        ? info.tpBuyPriceInCopper * TP_FEE_NUMERATOR / TP_FEE_DENOMINATOR : 0;
    if (useInstantSellPrice) {
        // Only the buy-order price is money in hand right now - the sell
        // listing requires waiting for a buyer to meet it.
        return std::max(vendorProfit, tpBuyProfit);
    }
    const int64_t tpSellProfit = info.tpSellPriceInCopper > 0
        ? info.tpSellPriceInCopper * TP_FEE_NUMERATOR / TP_FEE_DENOMINATOR : 0;
    return std::max({ vendorProfit, tpSellProfit, tpBuyProfit });
}
