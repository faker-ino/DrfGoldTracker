#include "Gw2Api.h"

#include <algorithm>
#include <chrono>
#include <sstream>

#include <ixwebsocket/IXHttpClient.h>
#include <nlohmann/json.hpp>

namespace {
constexpr int MAX_IDS_PER_BATCH = 200; // GW2 API's own per-request id limit
// How long a fetched price is trusted before EnsureItemsRequested queues a
// refresh for it again. Balances staying reasonably current against not
// hammering api.guildwars2.com for items that keep dropping repeatedly.
constexpr auto PRICE_REFRESH_INTERVAL = std::chrono::minutes(5);

std::string JoinIds(const std::vector<int64_t>& ids)
{
    std::ostringstream oss;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) oss << ',';
        oss << ids[i];
    }
    return oss.str();
}

// A non-2xx status (e.g. the 404 the prices endpoint returns for an all-
// non-tradable batch) is a normal, expected response from a reachable
// server - only a missing response or a transport-level error indicates
// the API itself is unreachable.
bool IsNetworkFailure(const ix::HttpResponsePtr& response)
{
    return !response || response->errorCode != ix::HttpErrorCode::Ok;
}

// GW2's v2 API returns 206 Partial Content (not 200) for a bulk ids= request
// that mixes recognized/tradable ids with ones it can't return a full entry
// for - e.g. commerce/prices for a batch that includes some untradeable
// items alongside tradable ones, which is the everyday case for a farming
// session (bound drops mixed with sellable materials). Requiring exactly
// 200 meant that single unrecognized id in a batch silently discarded the
// whole batch's response, including perfectly good price data for every
// other item in it.
bool IsSuccessStatus(int statusCode)
{
    return statusCode >= 200 && statusCode < 300;
}
} // namespace

Gw2Api::Gw2Api()
{
    Start();
}

Gw2Api::~Gw2Api()
{
    Stop(); // fallback - Stop() should already have been called from Unload()
}

void Gw2Api::Start()
{
    if (_worker.joinable()) {
        return; // already running
    }
    _stop = false;
    _workerFinished = false;
    _worker = std::thread(&Gw2Api::WorkerLoop, this);
}

void Gw2Api::Stop()
{
    _stop = true;
    {
        std::lock_guard<std::mutex> lock(_activeCancelMutex);
        if (_activeCancelFlag) {
            _activeCancelFlag->store(true);
        }
    }
    _pendingCv.notify_all();

    if (!_worker.joinable()) {
        return;
    }

    // Give the worker a couple of pending-queue polls' worth of time to
    // notice _stop and, if it was mid-request, react to the cancel flag
    // above. Anything longer than that means it's genuinely stuck (e.g. a
    // DNS resolve that doesn't respect the cancel flag) - detach rather
    // than block Unload() on it.
    constexpr auto kShutdownDeadline = std::chrono::seconds(2);
    const auto deadline = std::chrono::steady_clock::now() + kShutdownDeadline;
    while (!_workerFinished.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (_workerFinished.load()) {
        _worker.join();
    } else {
        _worker.detach();
    }
}

void Gw2Api::LoadPersistedItems(const std::unordered_map<int64_t, Gw2ItemInfo>& items)
{
    std::lock_guard<std::mutex> lock(_cacheMutex);
    for (const auto& [id, info] : items) {
        Gw2ItemInfo entry = info;
        entry.lastFetchedAt = {}; // treat as stale so it refreshes as soon as it's relevant this run
        _itemCache[id] = std::move(entry);
    }
}

void Gw2Api::EnsureItemsRequested(const std::vector<int64_t>& itemIds)
{
    bool addedAny = false;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> cacheLock(_cacheMutex);
        std::lock_guard<std::mutex> pendingLock(_pendingMutex);
        for (int64_t id : itemIds) {
            auto it = _itemCache.find(id);
            const bool needsFetch = (it == _itemCache.end()) || (now - it->second.lastFetchedAt >= PRICE_REFRESH_INTERVAL);
            if (needsFetch && _pendingItemIds.insert(id).second) {
                addedAny = true;
            }
        }
    }
    if (addedAny) {
        _pendingCv.notify_all();
    }
}

std::unordered_map<int64_t, Gw2ItemInfo> Gw2Api::GetAllCachedItems() const
{
    std::lock_guard<std::mutex> lock(_cacheMutex);
    return _itemCache;
}

Gw2ItemInfo Gw2Api::GetItemInfo(int64_t itemId) const
{
    std::lock_guard<std::mutex> lock(_cacheMutex);
    auto it = _itemCache.find(itemId);
    return it == _itemCache.end() ? Gw2ItemInfo{} : it->second;
}

Gw2CurrencyInfo Gw2Api::GetCurrencyInfo(int64_t currencyId) const
{
    std::lock_guard<std::mutex> lock(_cacheMutex);
    auto it = _currencyCache.find(currencyId);
    return it == _currencyCache.end() ? Gw2CurrencyInfo{} : it->second;
}

void Gw2Api::WorkerLoop()
{
    // An exception escaping a std::thread's entry function calls
    // std::terminate() and takes down the whole game process, not just this
    // addon - guard every iteration so a bad response or a library hiccup
    // becomes a skipped cycle instead of a crash.
    try {
        FetchCurrencies();
    } catch (...) {
        // will simply show "Currency <id>" fallback in the UI
    }

    while (!_stop) {
        std::vector<int64_t> batch;
        {
            std::unique_lock<std::mutex> lock(_pendingMutex);
            _pendingCv.wait_for(lock, std::chrono::seconds(2),
                                 [this] { return _stop.load() || !_pendingItemIds.empty(); });
            if (_stop) {
                return;
            }
            for (auto it = _pendingItemIds.begin(); it != _pendingItemIds.end() && batch.size() < MAX_IDS_PER_BATCH;) {
                batch.push_back(*it);
                it = _pendingItemIds.erase(it);
            }
        }

        if (!batch.empty()) {
            try {
                FetchItemBatch(batch);
            } catch (...) {
                // this batch's items just stay unresolved - EnsureItemsRequested
                // will queue them again on the next PRICE_REFRESH_INTERVAL pass
            }
        }
    }

    _workerFinished = true;
}

void Gw2Api::FetchCurrencies()
{
    ix::HttpClient http;
    auto args = http.createRequest();
    args->connectTimeout = 10;
    args->transferTimeout = 15;

    {
        std::lock_guard<std::mutex> lock(_activeCancelMutex);
        _activeCancelFlag = &args->cancel;
    }
    auto response = http.get("https://api.guildwars2.com/v2/currencies?ids=all", args);
    {
        std::lock_guard<std::mutex> lock(_activeCancelMutex);
        _activeCancelFlag = nullptr;
    }
    if (IsNetworkFailure(response)) {
        _consecutiveFailures.fetch_add(1);
        return; // will simply show "Currency <id>" fallback in the UI
    }
    _consecutiveFailures = 0;
    if (!IsSuccessStatus(response->statusCode)) {
        return; // will simply show "Currency <id>" fallback in the UI
    }

    try {
        nlohmann::json doc = nlohmann::json::parse(response->body);
        std::lock_guard<std::mutex> lock(_cacheMutex);
        for (const auto& entry : doc) {
            Gw2CurrencyInfo info;
            info.loaded = true;
            info.name = entry.value("name", "");
            info.iconUrl = entry.value("icon", "");
            _currencyCache[entry.at("id").get<int64_t>()] = std::move(info);
        }
    } catch (const nlohmann::json::exception&) {
        // leave cache empty - UI falls back to showing raw currency ids
    }
}

void Gw2Api::FetchItemBatch(const std::vector<int64_t>& ids)
{
    const std::string idList = JoinIds(ids);

    ix::HttpClient http;
    auto args = http.createRequest();
    args->connectTimeout = 10;
    args->transferTimeout = 15;

    // Start from whatever's already cached (e.g. restored from disk) rather
    // than blank, so a transient API hiccup on one of the two calls below
    // doesn't wipe out a name/price we already knew.
    std::unordered_map<int64_t, Gw2ItemInfo> fetched;
    {
        std::lock_guard<std::mutex> lock(_cacheMutex);
        for (int64_t id : ids) {
            auto it = _itemCache.find(id);
            fetched[id] = (it != _itemCache.end()) ? it->second : Gw2ItemInfo{};
            fetched[id].loaded = true; // mark all requested ids resolved even if the API omits/rejects some
        }
    }

    {
        std::lock_guard<std::mutex> lock(_activeCancelMutex);
        _activeCancelFlag = &args->cancel;
    }

    auto itemsResponse = http.get("https://api.guildwars2.com/v2/items?ids=" + idList, args);
    if (IsNetworkFailure(itemsResponse)) {
        _consecutiveFailures.fetch_add(1);
    } else {
        _consecutiveFailures = 0;
    }
    if (itemsResponse && itemsResponse->errorCode == ix::HttpErrorCode::Ok && IsSuccessStatus(itemsResponse->statusCode)) {
        try {
            nlohmann::json doc = nlohmann::json::parse(itemsResponse->body);
            for (const auto& entry : doc) {
                int64_t id = entry.at("id").get<int64_t>();
                fetched[id].name = entry.value("name", "");
                fetched[id].rarity = entry.value("rarity", "");
                fetched[id].iconUrl = entry.value("icon", "");
                fetched[id].vendorValue = entry.value("vendor_value", 0);
                const auto flags = entry.value("flags", nlohmann::json::array());
                fetched[id].noSell = std::find(flags.begin(), flags.end(), "NoSell") != flags.end();
                fetched[id].soulboundOnAcquire = std::find(flags.begin(), flags.end(), "SoulbindOnAcquire") != flags.end();
            }
        } catch (const nlohmann::json::exception&) {
            // partial/garbled response - keep whatever defaults were set above
        }
    }

    // Non-tradable ids make the whole batch 404 ("all ids provided are
    // invalid") - that's expected, not an error, so no special-casing here.
    auto pricesResponse = http.get("https://api.guildwars2.com/v2/commerce/prices?ids=" + idList, args);
    {
        std::lock_guard<std::mutex> lock(_activeCancelMutex);
        _activeCancelFlag = nullptr;
    }
    if (IsNetworkFailure(pricesResponse)) {
        _consecutiveFailures.fetch_add(1);
    } else {
        _consecutiveFailures = 0;
    }
    if (pricesResponse && pricesResponse->errorCode == ix::HttpErrorCode::Ok && IsSuccessStatus(pricesResponse->statusCode)) {
        try {
            nlohmann::json doc = nlohmann::json::parse(pricesResponse->body);
            for (const auto& entry : doc) {
                int64_t id = entry.at("id").get<int64_t>();
                if (entry.contains("buys")) {
                    // buys.unit_price is the highest current buy order - what
                    // you'd receive instant-selling into it.
                    const int64_t rawBuyPrice = entry.at("buys").value("unit_price", 0);
                    fetched[id].tpBuyPriceInCopper = rawBuyPrice > 0 ? rawBuyPrice : -1;
                }
                if (entry.contains("sells")) {
                    // sells.unit_price is the lowest active sell listing -
                    // what you'd pay instant-buying it. Kept alongside the
                    // buy price (rather than using only one) because
                    // EstimateItemUnitValueInCopper takes whichever of the
                    // two is higher, same as the reference tracker.
                    const int64_t rawSellPrice = entry.at("sells").value("unit_price", 0);
                    fetched[id].tpSellPriceInCopper = rawSellPrice > 0 ? rawSellPrice : -1;
                }
            }
        } catch (const nlohmann::json::exception&) {
            // no TP prices for this batch - items keep tpBuyPriceInCopper == -1
        }
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(_cacheMutex);
    for (auto& [id, info] : fetched) {
        info.lastFetchedAt = now; // don't re-queue for another PRICE_REFRESH_INTERVAL
        _itemCache[id] = std::move(info);
    }
}
