// TrackerSettings - all persisted user-configurable behavior in one place:
// window visibility, the DRF token, item sort/filter selections, ignored
// stat ids, and the automatic session-reset schedule. Consolidated into a
// struct (rather than main.cpp's original loose globals) now that there are
// ~15 independent settings that all get loaded/saved together.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

#include "ResetScheduler.h"

// One entry per sortable column in the Items table - kept in sync with
// clicking that table's headers (see RenderItemRows in main.cpp), not a
// standalone settings-panel control.
enum class SortKey {
    Name,
    Count,
    ProfitTotal, // the "Est. Value" column: count * per-unit estimated value
};

const char* SortKeyToString(SortKey value);
SortKey SortKeyFromString(const std::string& value, SortKey fallback = SortKey::ProfitTotal);

struct TrackerSettings {
    bool windowVisible = true;
    std::string drfToken;

    // Whether the addon's shortcut icon is shown in Nexus's QuickAccess bar.
    // Purely cosmetic - the window can still be toggled via the keybind
    // (see kToggleWindowKeybindId in main.cpp) regardless of this setting.
    bool showQuickAccessIcon = true;

    // When true, the Favorites/Currencies/Items/Ignored lists show only each
    // stat's icon (plus its count/value) instead of its name text - a more
    // compact view. The name is still available as a hover tooltip.
    bool iconOnlyView = false;

    // Icon-grid tile size under iconOnlyView, as a multiple of the current
    // ImGui font size (see GridTileSize() in main.cpp) - adjustable via the
    // +/- buttons shown next to the grid, rather than a fixed pixel size, so
    // it still scales sensibly if the user changes Nexus's UI scale.
    float iconGridTileScale = 2.4f;

    // Sorting - set by clicking the Items table's column headers (see
    // RenderItemRows in main.cpp), not a standalone settings-panel control.
    // A single key+direction, not a multi-key stack.
    SortKey sortKey = SortKey::ProfitTotal;
    bool sortDescending = true;

    // Est. Value column: by default a best-case value (see
    // EstimateItemUnitValueInCopper) that favors the TP sell-listing price.
    // When true, uses only the instant-sell (highest buy order) price -
    // what selling right now would actually pay.
    bool estValueUseInstantSellPrice = false;

    // Filters are exclude-lists: checking a box hides stats matching that
    // category, unchecked (the default) leaves it shown. Independent of
    // each other - a stat matching any checked flag is hidden, regardless
    // of what else is checked or unchecked.
    bool filterHideVendorSellable = false;
    bool filterHideTpSellable = false;
    bool filterHideNotSellable = false;
    bool filterHidePositiveCount = false;
    bool filterHideNegativeCount = false;
    bool filterHideKnownByApi = false;
    bool filterHideUnknownByApi = false;
    std::unordered_set<std::string> filterHiddenRarities; // empty = no rarity hidden

    // Ignored and Favorite are mutually exclusive per stat - main.cpp
    // enforces this by removing an id from the other set whenever it's
    // added to one, rather than these being independent flags.
    std::unordered_set<int64_t> ignoredItemIds;
    std::unordered_set<int64_t> ignoredCurrencyIds;
    std::unordered_set<int64_t> favoriteItemIds;
    std::unordered_set<int64_t> favoriteCurrencyIds;

    AutomaticReset automaticReset = AutomaticReset::Never;
    int minutesUntilResetAfterUnload = 60;
    // Epoch seconds (UTC) of the next scheduled automatic reset - persisted
    // so a MinutesAfterUnload countdown or a daily/weekly reset boundary
    // survives an addon reload or a game restart. 0 = not yet initialized.
    int64_t nextResetUtcEpochSeconds = 0;

    // Missing/malformed file (or missing individual fields, e.g. after an
    // upgrade) keeps the corresponding default rather than failing to load.
    static TrackerSettings LoadFromFile(const std::string& path);
    void SaveToFile(const std::string& path) const;
};
