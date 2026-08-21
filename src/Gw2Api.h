// Gw2Api - looks up item/currency names and trading-post buy prices from
// the public GW2 API (api.guildwars2.com) so the UI can show gold values
// instead of bare item ids. No API key needed - these are static/game-data
// endpoints.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct Gw2ItemInfo {
    bool loaded = false;
    std::string name;
    std::string rarity; // raw GW2 API rarity string ("Junk".."Legendary") - empty if unknown
    std::string iconUrl; // full render.guildwars2.com URL - empty if unknown
    int64_t vendorValue = 0;
    bool noSell = false; // item has the "NoSell" flag - vendorValue can't actually be realized
    int64_t tpSellPriceInCopper = -1; // lowest active sell listing ("sells.unit_price") - -1 = unknown/none
    int64_t tpBuyPriceInCopper = -1; // -1 = not tradable / unknown

    // Item has the "SoulbindOnAcquire" flag - unlike "AccountBound" (which
    // means no instance of the id was ever tradable, so commerce/prices
    // simply has no entry for it and tpSell/tpBuyPriceInCopper are already
    // -1), this id can be obtained both bound and unbound - e.g. crafted or
    // TP-purchased unbound, but soulbound the instant it drops from this
    // farming source. commerce/prices then reports a real market price from
    // other players' unbound copies, even though this specific drop can
    // never actually be listed - so that price must be excluded from this
    // item's estimated value rather than discounted by the usual TP fee.
    bool soulboundOnAcquire = false;

    // When the price was last fetched live - NOT persisted (always
    // time_point::min() right after LoadPersistedItems, so a restored item
    // is treated as due for a refresh as soon as it's relevant). Re-fetched
    // periodically rather than once, since TP prices drift over a session -
    // see PRICE_REFRESH_INTERVAL in Gw2Api.cpp.
    std::chrono::steady_clock::time_point lastFetchedAt{};
};

struct Gw2CurrencyInfo {
    bool loaded = false;
    std::string name;
    std::string iconUrl; // full render.guildwars2.com URL - empty if unknown
};

class Gw2Api {
public:
    Gw2Api();
    ~Gw2Api();

    Gw2Api(const Gw2Api&) = delete;
    Gw2Api& operator=(const Gw2Api&) = delete;

    // (Re)starts the worker thread. The constructor already calls this once,
    // but Stop() permanently ends that thread - Load() must call Start()
    // again on every re-enable (Nexus doesn't necessarily reload the DLL
    // between addon disable/enable), or lookups silently stop working after
    // the first Unload. Safe to call when already running (no-op).
    void Start();

    // Stops the worker thread. Must be called from Nexus's Unload() before
    // it calls FreeLibrary - joining a thread later, from this object's
    // destructor during DLL_PROCESS_DETACH, happens under the loader lock
    // and can hang the whole process instead of returning. Safe to call
    // more than once (the destructor also calls it, as a fallback).
    //
    // Bounded, not a blind join: the worker can be blocked inside a
    // synchronous HTTP call (connectTimeout+transferTimeout up to ~25s).
    // Stop() flips that request's cancel flag to unblock it quickly, but
    // falls back to detaching the thread after a short deadline rather than
    // joining indefinitely - Nexus calls this from Unload() on the main
    // thread, and stalling that for up to 25s on every disable is what let
    // fast repeated load/unload cycles pile up into a crash.
    void Stop();

    // Seeds the cache from a previous run's disk cache (see main.cpp's
    // Load/SaveItemCache) - call once right after construction, before
    // EnsureItemsRequested, so previously-seen items display their real
    // name instantly instead of "Item #<id>" while a fresh lookup runs.
    void LoadPersistedItems(const std::unordered_map<int64_t, Gw2ItemInfo>& items);

    // Called every frame with the item ids currently visible in the
    // session totals - queues up any ids whose price is missing or stale
    // (see PRICE_REFRESH_INTERVAL) for background lookup.
    void EnsureItemsRequested(const std::vector<int64_t>& itemIds);

    Gw2ItemInfo GetItemInfo(int64_t itemId) const;
    Gw2CurrencyInfo GetCurrencyInfo(int64_t currencyId) const;

    // Consecutive HTTP-level failures (network error/timeout - not just an
    // unexpected status code) across currency/item lookups, reset to 0 on
    // any successful round-trip. Lets the UI surface "API unreachable"
    // instead of leaving unresolved items stuck on "..." with no explanation.
    int GetConsecutiveFailures() const { return _consecutiveFailures.load(); }

    // Snapshot for persisting to disk - see main.cpp's SaveItemCache.
    std::unordered_map<int64_t, Gw2ItemInfo> GetAllCachedItems() const;

private:
    void WorkerLoop();
    void FetchCurrencies();
    void FetchItemBatch(const std::vector<int64_t>& ids);

    mutable std::mutex _cacheMutex;
    std::unordered_map<int64_t, Gw2ItemInfo> _itemCache;
    std::unordered_map<int64_t, Gw2CurrencyInfo> _currencyCache;

    std::mutex _pendingMutex;
    std::condition_variable _pendingCv;
    std::unordered_set<int64_t> _pendingItemIds; // requested but not yet fetched/known

    std::thread _worker;
    std::atomic<bool> _stop{false};
    std::atomic<bool> _workerFinished{false};

    // Points at the cancel flag of whichever HttpRequestArgs the worker is
    // currently blocked on inside http.get(), if any - lets Stop() abort an
    // in-flight request instead of waiting out its full timeout. Guarded by
    // _activeCancelMutex because it's written by the worker thread and read
    // by Stop() on whatever thread calls it (Nexus's Unload()).
    std::mutex _activeCancelMutex;
    std::atomic<bool>* _activeCancelFlag = nullptr;

    std::atomic<int> _consecutiveFailures{0};
};
