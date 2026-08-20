// IconCache - downloads GW2 API icon images to disk once and reuses the
// cached file on every subsequent lookup. Nexus's own
// Textures_GetOrCreateFromURL keeps textures in memory only for the current
// game session, so without a disk cache every addon reload / game restart
// re-downloads every icon from render.guildwars2.com from scratch.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

class IconCache {
public:
    IconCache();
    ~IconCache();

    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;

    // (Re)starts the worker thread - same Start()/Stop() re-entry pattern as
    // Gw2Api and for the same reason: Nexus can re-enable this addon
    // without reloading the DLL, so the process-lifetime instance needs to
    // be restartable.
    void Start();

    // Stops the worker thread - bounded wait then detach, not a blind join,
    // for the same reason as Gw2Api::Stop() (the worker can be mid-download
    // and Unload() must not stall the main thread waiting it out).
    void Stop();

    // Call on every Load(), before Start() - dir is where cached icon files
    // are written/read (e.g. "<addon dir>/icons"). Mutex-guarded (not just
    // "happens-before Start()") because Stop()'s bounded wait can fall back
    // to detaching a still-running worker - a fast re-Load() could then call
    // SetCacheDir() again while that detached worker is still reading it.
    void SetCacheDir(const std::string& dir);

    // Deterministic local path for this URL (content-addressed: the same
    // URL always maps to the same filename) - works uniformly for item and
    // currency icons without the caller needing to know which kind it is.
    std::string GetLocalPath(const std::string& iconUrl) const;

    // True if the icon is already cached to disk. Called from the render
    // thread for every visible row, every frame - a raw
    // std::filesystem::exists() at that rate turned out to cost real,
    // measured frame time (a live game process's filesystem calls aren't as
    // cheap as a synthetic benchmark suggests, especially with AV/EDR
    // hooking file opens). Once a URL is confirmed on disk it can never
    // become un-cached from under us (nothing in this class deletes cached
    // files), so the result is memoized in memory and the filesystem is
    // only actually touched once per icon, ever.
    bool IsCached(const std::string& iconUrl) const;

    // Queues a background download if iconUrl isn't cached yet, isn't
    // already queued/downloading, and isn't cooling down after a recent
    // failed attempt (see ICON_RETRY_COOLDOWN in the .cpp). Safe to call
    // every frame with the same URL repeatedly - a persistent failure
    // retries periodically rather than either spamming every frame or
    // getting stuck forever after one transient failure (a single dropped
    // request used to mean that icon just never loaded for the rest of the
    // game session, until the addon was reloaded).
    void RequestDownload(const std::string& iconUrl);

private:
    void WorkerLoop();
    bool DownloadToCache(const std::string& iconUrl); // returns whether it's cached to disk afterward

    mutable std::mutex _cacheDirMutex;
    std::string _cacheDir; // guarded by _cacheDirMutex - see SetCacheDir's comment

    mutable std::mutex _confirmedCachedMutex;
    mutable std::unordered_set<std::string> _confirmedCachedUrls; // see IsCached()'s comment

    std::mutex _pendingMutex;
    std::condition_variable _pendingCv;
    std::unordered_set<std::string> _pendingUrls;  // queued, not yet started
    std::unordered_set<std::string> _inFlightUrls; // currently downloading
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> _nextRetryAt; // set only after a failed attempt

    std::thread _worker;
    std::atomic<bool> _stop{false};
    std::atomic<bool> _workerFinished{false};

    // Same rationale as Gw2Api::_activeCancelFlag: lets Stop() abort an
    // in-flight download instead of waiting out its full timeout.
    std::mutex _activeCancelMutex;
    std::atomic<bool>* _activeCancelFlag = nullptr;
};
