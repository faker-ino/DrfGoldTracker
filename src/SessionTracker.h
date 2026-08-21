// SessionTracker - accumulates raw item/currency deltas coming from DRF
// drop messages into running per-session totals. Thread-safe: DrfClient
// feeds it from its receive thread, main.cpp reads it from the render
// thread.
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

class SessionTracker {
public:
    // GW2 currency id for coin (gold/silver/copper), per the v2/currencies API.
    static constexpr int64_t COIN_CURRENCY_ID = 1;

    void ApplyDrop(const std::unordered_map<int64_t, int64_t>& itemDeltas,
                    const std::unordered_map<int64_t, int64_t>& currencyDeltas);

    void Reset();

    // Restores state saved by a previous run (see main.cpp's
    // Load/SaveSessionState) - call once right after construction, before
    // any ApplyDrop. priorElapsedSeconds carries duration forward across
    // restarts since steady_clock's epoch resets on every process start.
    void LoadPersisted(const std::unordered_map<int64_t, int64_t>& itemTotals,
                        const std::unordered_map<int64_t, int64_t>& currencyTotals,
                        int64_t priorElapsedSeconds);

    // Snapshot accessors - copy out under lock so the caller can render
    // without holding the tracker's mutex.
    std::unordered_map<int64_t, int64_t> GetItemTotals() const;
    std::unordered_map<int64_t, int64_t> GetCurrencyTotals() const;
    int64_t GetCoinTotal() const;
    int64_t GetElapsedSeconds() const;

private:
    mutable std::mutex _mutex;
    std::unordered_map<int64_t, int64_t> _itemTotals;
    std::unordered_map<int64_t, int64_t> _currencyTotals;
    std::chrono::steady_clock::time_point _sessionStart = std::chrono::steady_clock::now();
    int64_t _priorElapsedSeconds = 0;
};
