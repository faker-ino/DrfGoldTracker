#include "SessionTracker.h"

void SessionTracker::ApplyDrop(const std::unordered_map<int64_t, int64_t>& itemDeltas,
                                const std::unordered_map<int64_t, int64_t>& currencyDeltas)
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (const auto& [id, delta] : itemDeltas) {
        _itemTotals[id] += delta;
    }
    for (const auto& [id, delta] : currencyDeltas) {
        _currencyTotals[id] += delta;
    }
}

void SessionTracker::Reset()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _itemTotals.clear();
    _currencyTotals.clear();
    _sessionStart = std::chrono::steady_clock::now();
    _priorElapsedSeconds = 0;
}

void SessionTracker::LoadPersisted(const std::unordered_map<int64_t, int64_t>& itemTotals,
                                    const std::unordered_map<int64_t, int64_t>& currencyTotals,
                                    int64_t priorElapsedSeconds)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _itemTotals = itemTotals;
    _currencyTotals = currencyTotals;
    _priorElapsedSeconds = priorElapsedSeconds;
    _sessionStart = std::chrono::steady_clock::now();
}

std::unordered_map<int64_t, int64_t> SessionTracker::GetItemTotals() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _itemTotals;
}

std::unordered_map<int64_t, int64_t> SessionTracker::GetCurrencyTotals() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _currencyTotals;
}

int64_t SessionTracker::GetCoinTotal() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _currencyTotals.find(COIN_CURRENCY_ID);
    return it == _currencyTotals.end() ? 0 : it->second;
}

int64_t SessionTracker::GetElapsedSeconds() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto sinceStart = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - _sessionStart).count();
    return _priorElapsedSeconds + sinceStart;
}
