#include "TrackerSettings.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

const char* SortKeyToString(SortKey value)
{
    switch (value) {
        case SortKey::Name:        return "name";
        case SortKey::Count:       return "count";
        case SortKey::ProfitTotal: return "profit_total";
    }
    return "profit_total";
}

SortKey SortKeyFromString(const std::string& value, SortKey fallback)
{
    if (value == "name") return SortKey::Name;
    if (value == "count") return SortKey::Count;
    if (value == "profit_total") return SortKey::ProfitTotal;
    // "profit_per_item"/"rarity"/"api_id" are values from a settings.json
    // written before the Sorting options panel was replaced by clickable
    // table headers - just fall back rather than failing to load.
    return fallback;
}

namespace {

nlohmann::json StringSetToJson(const std::unordered_set<std::string>& set)
{
    return nlohmann::json(std::vector<std::string>(set.begin(), set.end()));
}

std::unordered_set<std::string> JsonToStringSet(const nlohmann::json& array)
{
    std::unordered_set<std::string> set;
    if (!array.is_array()) {
        return set;
    }
    for (const auto& entry : array) {
        if (entry.is_string()) {
            set.insert(entry.get<std::string>());
        }
    }
    return set;
}

nlohmann::json IdSetToJson(const std::unordered_set<int64_t>& set)
{
    return nlohmann::json(std::vector<int64_t>(set.begin(), set.end()));
}

std::unordered_set<int64_t> JsonToIdSet(const nlohmann::json& array)
{
    std::unordered_set<int64_t> set;
    if (!array.is_array()) {
        return set;
    }
    for (const auto& entry : array) {
        try {
            set.insert(entry.get<int64_t>());
        } catch (const nlohmann::json::exception&) {
            // malformed entry - skip it rather than failing the whole load
        }
    }
    return set;
}

} // namespace

TrackerSettings TrackerSettings::LoadFromFile(const std::string& path)
{
    TrackerSettings settings; // defaults, used as-is if the file is missing/malformed
    std::ifstream file(path);
    if (!file.is_open()) {
        return settings;
    }
    try {
        nlohmann::json doc;
        file >> doc;

        settings.windowVisible = doc.value("window_visible", settings.windowVisible);
        settings.drfToken = doc.value("drf_token", settings.drfToken);
        settings.showQuickAccessIcon = doc.value("show_quick_access_icon", settings.showQuickAccessIcon);
        settings.iconOnlyView = doc.value("icon_only_view", settings.iconOnlyView);
        settings.iconGridTileScale = doc.value("icon_grid_tile_scale", settings.iconGridTileScale);

        settings.sortKey = SortKeyFromString(doc.value("sort_key", std::string()), settings.sortKey);
        settings.sortDescending = doc.value("sort_descending", settings.sortDescending);

        settings.estValueUseInstantSellPrice = doc.value("est_value_use_instant_sell_price", settings.estValueUseInstantSellPrice);

        if (doc.contains("filter") && doc["filter"].is_object()) {
            const auto& filter = doc["filter"];
            settings.filterHideVendorSellable = filter.value("hide_vendor_sellable", false);
            settings.filterHideTpSellable = filter.value("hide_tp_sellable", false);
            settings.filterHideNotSellable = filter.value("hide_not_sellable", false);
            settings.filterHidePositiveCount = filter.value("hide_positive_count", false);
            settings.filterHideNegativeCount = filter.value("hide_negative_count", false);
            settings.filterHideKnownByApi = filter.value("hide_known_by_api", false);
            settings.filterHideUnknownByApi = filter.value("hide_unknown_by_api", false);
            if (filter.contains("hidden_rarities")) {
                settings.filterHiddenRarities = JsonToStringSet(filter["hidden_rarities"]);
            }
        }

        if (doc.contains("ignored_item_ids")) {
            settings.ignoredItemIds = JsonToIdSet(doc["ignored_item_ids"]);
        }
        if (doc.contains("ignored_currency_ids")) {
            settings.ignoredCurrencyIds = JsonToIdSet(doc["ignored_currency_ids"]);
        }
        if (doc.contains("favorite_item_ids")) {
            settings.favoriteItemIds = JsonToIdSet(doc["favorite_item_ids"]);
        }
        if (doc.contains("favorite_currency_ids")) {
            settings.favoriteCurrencyIds = JsonToIdSet(doc["favorite_currency_ids"]);
        }

        settings.automaticReset = AutomaticResetFromString(doc.value("automatic_reset", std::string()), settings.automaticReset);
        settings.minutesUntilResetAfterUnload = doc.value("minutes_until_reset_after_unload", settings.minutesUntilResetAfterUnload);
        settings.nextResetUtcEpochSeconds = doc.value("next_reset_utc_epoch_seconds", settings.nextResetUtcEpochSeconds);
    } catch (const nlohmann::json::exception&) {
        return TrackerSettings{}; // malformed file - fall back to full defaults
    }
    return settings;
}

void TrackerSettings::SaveToFile(const std::string& path) const
{
    nlohmann::json doc;
    doc["window_visible"] = windowVisible;
    doc["drf_token"] = drfToken;
    doc["show_quick_access_icon"] = showQuickAccessIcon;
    doc["icon_only_view"] = iconOnlyView;
    doc["icon_grid_tile_scale"] = iconGridTileScale;

    doc["sort_key"] = SortKeyToString(sortKey);
    doc["sort_descending"] = sortDescending;

    doc["est_value_use_instant_sell_price"] = estValueUseInstantSellPrice;

    nlohmann::json filter;
    filter["hide_vendor_sellable"] = filterHideVendorSellable;
    filter["hide_tp_sellable"] = filterHideTpSellable;
    filter["hide_not_sellable"] = filterHideNotSellable;
    filter["hide_positive_count"] = filterHidePositiveCount;
    filter["hide_negative_count"] = filterHideNegativeCount;
    filter["hide_known_by_api"] = filterHideKnownByApi;
    filter["hide_unknown_by_api"] = filterHideUnknownByApi;
    filter["hidden_rarities"] = StringSetToJson(filterHiddenRarities);
    doc["filter"] = std::move(filter);

    doc["ignored_item_ids"] = IdSetToJson(ignoredItemIds);
    doc["ignored_currency_ids"] = IdSetToJson(ignoredCurrencyIds);
    doc["favorite_item_ids"] = IdSetToJson(favoriteItemIds);
    doc["favorite_currency_ids"] = IdSetToJson(favoriteCurrencyIds);

    doc["automatic_reset"] = AutomaticResetToString(automaticReset);
    doc["minutes_until_reset_after_unload"] = minutesUntilResetAfterUnload;
    doc["next_reset_utc_epoch_seconds"] = nextResetUtcEpochSeconds;

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

    std::ofstream file(path, std::ios::trunc);
    if (file.is_open()) {
        file << doc.dump(2);
    }
}
