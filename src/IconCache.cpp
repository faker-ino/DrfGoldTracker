#include "IconCache.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

#include <ixwebsocket/IXHttpClient.h>

IconCache::IconCache()
{
    Start();
}

IconCache::~IconCache()
{
    Stop(); // fallback - Stop() should already have been called from Unload()
}

void IconCache::Start()
{
    if (_worker.joinable()) {
        return; // already running
    }
    _stop = false;
    _workerFinished = false;
    _worker = std::thread(&IconCache::WorkerLoop, this);
}

void IconCache::Stop()
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

void IconCache::SetCacheDir(const std::string& dir)
{
    {
        std::lock_guard<std::mutex> lock(_cacheDirMutex);
        _cacheDir = dir;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
}

std::string IconCache::GetLocalPath(const std::string& iconUrl) const
{
    std::string cacheDir;
    {
        std::lock_guard<std::mutex> lock(_cacheDirMutex);
        cacheDir = _cacheDir;
    }
    if (cacheDir.empty() || iconUrl.empty()) {
        return {};
    }
    // The URL itself already uniquely identifies the icon (GW2's render
    // service embeds a content hash in the path) - hashing it down to a
    // filename just keeps paths short and filesystem-safe.
    std::ostringstream oss;
    oss << std::hex << std::hash<std::string>{}(iconUrl);
    return (std::filesystem::path(cacheDir) / (oss.str() + ".png")).string();
}

bool IconCache::IsCached(const std::string& iconUrl) const
{
    if (iconUrl.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(_confirmedCachedMutex);
        if (_confirmedCachedUrls.count(iconUrl)) {
            return true;
        }
    }
    std::error_code ec;
    const bool exists = std::filesystem::exists(GetLocalPath(iconUrl), ec);
    if (exists) {
        std::lock_guard<std::mutex> lock(_confirmedCachedMutex);
        _confirmedCachedUrls.insert(iconUrl);
    }
    return exists;
}

namespace {
// How long a failed download waits before RequestDownload will queue that
// URL again. Long enough that a persistently-broken URL (e.g. a 404) isn't
// retried every single frame it's on screen, short enough that a transient
// hiccup (a dropped connection, a momentary render.guildwars2.com blip)
// recovers within the same game session instead of needing an addon reload.
constexpr auto ICON_RETRY_COOLDOWN = std::chrono::seconds(30);
} // namespace

void IconCache::RequestDownload(const std::string& iconUrl)
{
    if (iconUrl.empty() || IsCached(iconUrl)) {
        return;
    }

    bool addedAny = false;
    {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        if (_pendingUrls.count(iconUrl) || _inFlightUrls.count(iconUrl)) {
            return; // already queued or currently downloading
        }
        const auto retryIt = _nextRetryAt.find(iconUrl);
        if (retryIt != _nextRetryAt.end() && std::chrono::steady_clock::now() < retryIt->second) {
            return; // failed recently - still cooling down
        }
        _pendingUrls.insert(iconUrl);
        addedAny = true;
    }
    if (addedAny) {
        _pendingCv.notify_all();
    }
}

void IconCache::WorkerLoop()
{
    // An exception escaping a std::thread's entry function calls
    // std::terminate() and takes down the whole game process - guard every
    // iteration so a bad response or a library hiccup becomes a skipped
    // icon instead of a crash.
    while (!_stop) {
        std::string url;
        {
            std::unique_lock<std::mutex> lock(_pendingMutex);
            _pendingCv.wait_for(lock, std::chrono::seconds(2),
                                 [this] { return _stop.load() || !_pendingUrls.empty(); });
            if (_stop) {
                break;
            }
            if (_pendingUrls.empty()) {
                continue;
            }
            auto it = _pendingUrls.begin();
            url = *it;
            _pendingUrls.erase(it);
            _inFlightUrls.insert(url);
        }

        bool succeeded = false;
        try {
            succeeded = DownloadToCache(url);
        } catch (...) {
            succeeded = false;
        }

        {
            std::lock_guard<std::mutex> lock(_pendingMutex);
            _inFlightUrls.erase(url);
            if (succeeded) {
                _nextRetryAt.erase(url);
            } else {
                _nextRetryAt[url] = std::chrono::steady_clock::now() + ICON_RETRY_COOLDOWN;
            }
        }
    }

    _workerFinished = true;
}

bool IconCache::DownloadToCache(const std::string& iconUrl)
{
    const std::string path = GetLocalPath(iconUrl);
    if (path.empty()) {
        return false; // SetCacheDir() was never called
    }

    std::error_code existsEc;
    if (std::filesystem::exists(path, existsEc)) {
        return true; // cached by an earlier run (or another queued request for the same URL)
    }

    ix::HttpClient http;
    auto args = http.createRequest();
    args->connectTimeout = 10;
    args->transferTimeout = 15;

    {
        std::lock_guard<std::mutex> lock(_activeCancelMutex);
        _activeCancelFlag = &args->cancel;
    }
    auto response = http.get(iconUrl, args);
    {
        std::lock_guard<std::mutex> lock(_activeCancelMutex);
        _activeCancelFlag = nullptr;
    }

    if (!response || response->errorCode != ix::HttpErrorCode::Ok || response->statusCode != 200) {
        return false; // RequestDownload will retry after ICON_RETRY_COOLDOWN
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file.write(response->body.data(), static_cast<std::streamsize>(response->body.size()));
    return true;
}
