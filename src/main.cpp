// main.cpp - Nexus addon entry point.
//
// This is a Nexus addon (a Windows x64 DLL, not a standalone executable):
// Nexus discovers it via the exported GetAddonDef(), hands it a live
// ImGuiContext + AddonAPI_t on Load, and calls Unload before unloading the
// DLL. See vendor/nexus/Nexus.h for the full API surface.
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <userenv.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "userenv.lib")

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <ixwebsocket/IXNetSystem.h>
#include <nlohmann/json.hpp>

#include "imgui.h"
#include "Nexus.h"

#include "DrfClient.h"
#include "Gw2Api.h"
#include "IconCache.h"
#include "Profit.h"
#include "ResetScheduler.h"
#include "SessionTracker.h"
#include "TrackerSettings.h"
#include "QuickAccessIconNormal.h"
#include "QuickAccessIconHover.h"
#include "GoldCoinIcon.h"
#include "SilverCoinIcon.h"
#include "CopperCoinIcon.h"

static AddonDefinition_t g_addonDef = {};
static AddonAPI_t*       g_api      = nullptr;
static HMODULE           g_hSelf    = nullptr;

// QuickAccess shortcut identifiers - the icon textures ship baked into the
// DLL as C++ byte arrays (see cmake/EmbedIcon.cmake and
// QuickAccessIconNormal.h/QuickAccessIconHover.h, generated at build time
// from resources/icons/) and are registered via Textures_GetOrCreateFromMemory
// rather than Win32 RCDATA + Textures_GetOrCreateFromResource - the RCDATA
// route was tried in an earlier addon and Nexus's loader couldn't find a
// resource Win32 itself confirmed was present in the DLL.
static constexpr const char* kQuickAccessShortcutId  = "DGT_QuickAccessShortcut";
static constexpr const char* kQuickAccessIconId      = "DGT_QuickAccessIcon";
static constexpr const char* kQuickAccessIconHoverId = "DGT_QuickAccessIconHover";
static constexpr const char* kToggleWindowKeybindId  = "DGT_ToggleWindow";

// Same embedded-texture pattern as the QuickAccess icons above, for the
// gold/silver/copper coin images used by RenderCoin (see its comment).
static constexpr const char* kGoldCoinIconId   = "DGT_GoldCoinIcon";
static constexpr const char* kSilverCoinIconId = "DGT_SilverCoinIcon";
static constexpr const char* kCopperCoinIconId = "DGT_CopperCoinIcon";

static TrackerSettings g_settings;
static std::string     g_settingsPath;
static std::string     g_sessionPath;

// Options panel token textbox state. File-scope (not function-local static)
// so Load() can re-sync it from g_settings.drfToken on every enable, instead
// of it permanently freezing after the first time AddonOptions() ever ran.
static char g_tokenBuf[256] = {};
static bool g_showToken = false;

static DrfClient      g_drfClient;
static Gw2Api          g_gw2Api;
static IconCache       g_iconCache;
static SessionTracker  g_sessionTracker;

// Serializes Load()/Unload() against each other and against themselves.
// Nexus's addon-list UI doesn't appear to block a second click while a
// disable is still in progress (Gw2Api::Stop()'s bounded wait can take up
// to ~2s) - a second Unload() (or a Load() from a fast re-enable) can then
// start while the first is still running, racing on the same non-thread-
// safe std::thread/ix::WebSocket objects and the settings/session files.
// Recursive so a same-thread reentrant call (e.g. Nexus pumping its own UI
// from inside a call it made into this DLL, which is exactly what spamming
// the disable toggle mid-disable triggers) doesn't deadlock. That reentrant
// case used to fall through and run Load()/Unload() a second time on top of
// the first, un-serialized, on the same objects - the actual cause of a
// crash from clicking disable repeatedly while it was still disabling.
// g_lifecycleDepth (see below) turns that case into a no-op instead;
// genuine concurrent calls from separate threads still block on this mutex
// and run fully serialized, one after the other.
static std::recursive_mutex g_lifecycleMutex;

// >0 while a Load()/Unload() call is executing; only ever touched while
// holding g_lifecycleMutex. Because the mutex above is recursive, a second
// Load()/Unload() invocation arriving on the SAME thread while depth is
// already >0 acquires the lock immediately instead of blocking - that can
// only happen via same-thread reentrancy (a genuinely different thread
// would still be blocked waiting on the mutex), so such a call bails out
// immediately rather than re-running teardown/startup concurrently with
// the outer call still in progress.
static int g_lifecycleDepth = 0;

// True while a Load() has completed and no matching Unload() has run yet.
// Only ever touched while holding g_lifecycleMutex. g_lifecycleDepth alone
// only catches a call stack-nested reentry (Unload() calling back into
// Unload() before the outer call returns); it does nothing for two
// back-to-back, non-nested Unload() calls (e.g. Nexus delivering several
// queued clicks as separate calls in a row rather than a nested one) - the
// second of those would still re-run teardown (double GUI_Deregister,
// double Stop()) against state the first call already tore down. This flag
// makes Load()/Unload() idempotent against that: a second Unload() while
// already unloaded, or a second Load() while already loaded, is a no-op.
static bool g_loaded = false;

static void AddonRender();
static void AddonOptions();
static void OpenGw2WikiPage(const std::string& itemName);
static void ToggleWindowKeybindHandler(const char* aIdentifier, bool aIsRelease);

// Set whenever something that affects the Favorites/Items rows or the
// Profit total changes (a settings save, a new drop) - see RefreshItemView
// and its throttled call site in AddonRenderImpl for why this exists:
// rebuilding those views is O(session item count), so it happens on a short
// timer instead of every frame, with this flag forcing an immediate rebuild
// so interactive changes (favorite/ignore, sort click, reset) still feel
// instant rather than waiting out the timer.
static bool s_itemViewDirty = true;

static void SaveSettings()
{
    s_itemViewDirty = true;
    if (g_settingsPath.empty()) {
        return;
    }
    g_settings.SaveToFile(g_settingsPath);
}

// Bound to the QuickAccess shortcut (and whatever keybind the user assigns
// it in Nexus's keybind options, since it's registered the normal way) -
// fires on both press and release, so this only acts on the press half to
// avoid toggling twice per click.
static void ToggleWindowKeybindHandler(const char* /*aIdentifier*/, bool aIsRelease)
{
    if (aIsRelease) {
        return;
    }
    // Nexus can deliver a queued keypress on its own input thread - guard
    // against it landing mid-Load()/Unload() the same way AddonRender/
    // AddonOptions do (see their comment).
    std::unique_lock<std::recursive_mutex> lifecycleLock(g_lifecycleMutex, std::try_to_lock);
    if (!lifecycleLock.owns_lock() || !g_loaded) {
        return;
    }
    g_settings.windowVisible = !g_settings.windowVisible;
    SaveSettings();
}

// Session totals only live in SessionTracker's memory otherwise - an addon
// reload or game crash would silently wipe a farming session's progress.
// Persisted separately from settings.json since it's saved much more often
// (autosaved periodically, not just on explicit user action).
static nlohmann::json IdDeltaMapToJson(const std::unordered_map<int64_t, int64_t>& map)
{
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& [id, value] : map) {
        obj[std::to_string(id)] = value;
    }
    return obj;
}

static std::unordered_map<int64_t, int64_t> JsonToIdDeltaMap(const nlohmann::json& obj)
{
    std::unordered_map<int64_t, int64_t> map;
    if (!obj.is_object()) {
        return map;
    }
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        try {
            map[std::stoll(it.key())] = it.value().get<int64_t>();
        } catch (const std::exception&) {
            // malformed entry - skip it rather than failing the whole load
        }
    }
    return map;
}

static void LoadSessionState(const std::string& path)
{
    g_sessionPath = path;
    std::ifstream file(path);
    if (!file.is_open()) {
        return; // no prior session saved - start fresh
    }
    try {
        nlohmann::json doc;
        file >> doc;
        g_sessionTracker.LoadPersisted(
            doc.contains("items") ? JsonToIdDeltaMap(doc["items"]) : std::unordered_map<int64_t, int64_t>{},
            doc.contains("currencies") ? JsonToIdDeltaMap(doc["currencies"]) : std::unordered_map<int64_t, int64_t>{},
            doc.value("elapsed_seconds", (int64_t)0));
    } catch (const nlohmann::json::exception&) {
        // malformed file - start fresh rather than failing addon load
    }
}

static void SaveSessionState()
{
    if (g_sessionPath.empty()) {
        return;
    }
    nlohmann::json doc;
    doc["elapsed_seconds"] = g_sessionTracker.GetElapsedSeconds();
    doc["items"] = IdDeltaMapToJson(g_sessionTracker.GetItemTotals());
    doc["currencies"] = IdDeltaMapToJson(g_sessionTracker.GetCurrencyTotals());

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(g_sessionPath).parent_path(), ec);

    std::ofstream file(g_sessionPath, std::ios::trunc);
    if (file.is_open()) {
        file << doc.dump(2);
    }
}

// Item names/vendor values/rarity are static game data - caching them to
// disk means a previously-seen item shows its real name immediately on the
// next login instead of "Item #<id>" for the few seconds it takes the
// background lookup to run. TP prices still get refreshed live (see
// Gw2Api::PRICE_REFRESH_INTERVAL), this cache is just a warm starting point.
static std::string g_itemCachePath;

static void LoadItemCache(const std::string& path)
{
    g_itemCachePath = path;
    std::ifstream file(path);
    if (!file.is_open()) {
        return; // no cache yet - items will resolve at their normal pace
    }
    try {
        nlohmann::json doc;
        file >> doc;
        std::unordered_map<int64_t, Gw2ItemInfo> items;
        for (auto it = doc.begin(); it != doc.end(); ++it) {
            try {
                Gw2ItemInfo info;
                info.loaded = true;
                info.name = it.value().value("name", "");
                info.rarity = it.value().value("rarity", "");
                info.iconUrl = it.value().value("icon_url", "");
                info.vendorValue = it.value().value("vendor_value", (int64_t)0);
                info.noSell = it.value().value("no_sell", false);
                info.soulboundOnAcquire = it.value().value("soulbound_on_acquire", false);
                info.tpSellPriceInCopper = it.value().value("tp_sell_price", (int64_t)-1);
                info.tpBuyPriceInCopper = it.value().value("tp_buy_price", (int64_t)-1);
                items[std::stoll(it.key())] = std::move(info);
            } catch (const std::exception&) {
                // malformed entry - skip it rather than failing the whole load
            }
        }
        g_gw2Api.LoadPersistedItems(items);
    } catch (const nlohmann::json::exception&) {
        // malformed file - items will just resolve at their normal pace
    }
}

static void SaveItemCache()
{
    if (g_itemCachePath.empty()) {
        return;
    }
    nlohmann::json doc = nlohmann::json::object();
    for (const auto& [id, info] : g_gw2Api.GetAllCachedItems()) {
        if (!info.loaded || info.name.empty()) {
            continue; // nothing useful to remember for this id yet
        }
        nlohmann::json entry;
        entry["name"] = info.name;
        entry["rarity"] = info.rarity;
        entry["icon_url"] = info.iconUrl;
        entry["vendor_value"] = info.vendorValue;
        entry["no_sell"] = info.noSell;
        entry["soulbound_on_acquire"] = info.soulboundOnAcquire;
        entry["tp_sell_price"] = info.tpSellPriceInCopper;
        entry["tp_buy_price"] = info.tpBuyPriceInCopper;
        doc[std::to_string(id)] = std::move(entry);
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(g_itemCachePath).parent_path(), ec);

    std::ofstream file(g_itemCachePath, std::ios::trunc);
    if (file.is_open()) {
        file << doc.dump(2);
    }
}

// SaveSessionState()/SaveItemCache() both do blocking file I/O sized to the
// whole session and the whole item cache - the latter only ever grows across
// restarts (every item ever seen stays cached forever). Calling them inline
// from the render callback (as a plain 30s timer used to) stalls the game's
// render thread for however long that write takes, which shows up as a
// periodic hitch - felt most as janky scrolling since it lands mid-frame at
// an unpredictable moment. Running them from a dedicated thread that sleeps
// between ticks keeps that cost off the render thread entirely. Same
// Start()/Stop() re-entry shape as Gw2Api/IconCache since Load()/Unload()
// can run again without the DLL reloading.
static std::thread             g_autosaveThread;
static std::mutex              g_autosaveMutex;
static std::condition_variable g_autosaveCv;
static std::atomic<bool>       g_autosaveStop{false};

static void AutosaveWorkerLoop()
{
    std::unique_lock<std::mutex> lock(g_autosaveMutex);
    while (!g_autosaveCv.wait_for(lock, std::chrono::seconds(30), [] { return g_autosaveStop.load(); })) {
        lock.unlock();
        try {
            SaveSessionState();
            SaveItemCache();
        } catch (...) {
            // best-effort - a missed tick isn't fatal, the next one catches up
        }
        lock.lock();
    }
}

static void StartAutosaveThread()
{
    if (g_autosaveThread.joinable()) {
        return; // already running
    }
    g_autosaveStop = false;
    g_autosaveThread = std::thread(AutosaveWorkerLoop);
}

// Blind join, not IconCache's bounded-wait-then-detach - a save is a plain
// local file write with nothing to time out on, so it finishes almost
// immediately once woken, and this must complete (not detach) before the
// Unload()-time save below runs, or the two could race writing the same file.
static void StopAutosaveThread()
{
    {
        std::lock_guard<std::mutex> lock(g_autosaveMutex);
        g_autosaveStop = true;
    }
    g_autosaveCv.notify_all();
    if (g_autosaveThread.joinable()) {
        g_autosaveThread.join();
    }
}

// Unlike g_drfClient/g_gw2Api/g_iconCache (each a class instance whose own
// destructor guarantees Stop() runs), g_autosaveThread is a bare std::thread
// with no wrapping object - StopAutosaveThread() only ever runs from
// Unload()'s first try block. If that block throws before reaching it
// (caught further down, so Unload() still "completes"), or if Nexus tears
// down the DLL at process exit without calling Unload() at all, this thread
// is left joinable. std::thread's destructor calls std::terminate() (abort())
// on a still-joinable thread rather than quietly joining/detaching it - this
// guard's destructor is the safety net: as a static declared after the
// autosave globals, it's destroyed before them (reverse declaration order
// within the same translation unit), so it gets one last chance to stop and
// join the thread before it would otherwise reach a bare, unsafe destructor.
static struct AutosaveThreadShutdownGuard {
    ~AutosaveThreadShutdownGuard() { StopAutosaveThread(); }
} g_autosaveThreadShutdownGuard;

// Recomputes and persists g_settings.nextResetUtcEpochSeconds for the
// currently selected schedule. Never/OnAddonLoad have no fixed future
// timestamp to wait for (see ResetScheduler::ComputeNextResetUtc), so their
// "next reset" is stored as 0 (meaning "not applicable") rather than an
// unrepresentable time_point::max().
static void RecomputeNextResetDateTime()
{
    const auto next = ResetScheduler::ComputeNextResetUtc(
        std::chrono::system_clock::now(), g_settings.automaticReset, g_settings.minutesUntilResetAfterUnload);
    g_settings.nextResetUtcEpochSeconds = (next == std::chrono::system_clock::time_point::max())
        ? 0
        : std::chrono::duration_cast<std::chrono::seconds>(next.time_since_epoch()).count();
}

// Checks whether the configured automatic-reset schedule is due and, if so,
// resets the session and schedules the following one. Called once from
// Load() (isAddonLoad = true, to catch OnAddonLoad/MinutesAfterUnload) and
// then periodically from the render loop (isAddonLoad = false, to catch a
// daily/weekly reset boundary crossed mid-session).
static void CheckAndApplyAutomaticReset(bool isAddonLoad)
{
    using namespace std::chrono;

    if (g_settings.automaticReset == AutomaticReset::Never) {
        return;
    }

    // A freshly-selected schedule (or a settings.json from before this
    // feature existed) has no target yet - set one without resetting.
    // OnAddonLoad has no fixed future timestamp (it's evaluated directly
    // below via isAddonLoad), so it never needs this initialization.
    if (g_settings.nextResetUtcEpochSeconds == 0 && g_settings.automaticReset != AutomaticReset::OnAddonLoad) {
        RecomputeNextResetDateTime();
        SaveSettings();
        return;
    }

    bool dueNow = false;
    switch (g_settings.automaticReset) {
        case AutomaticReset::OnAddonLoad:
            dueNow = isAddonLoad;
            break;
        case AutomaticReset::MinutesAfterUnload:
            // The countdown is armed on Unload() - only ever due right after
            // the following Load(), not mid-session.
            dueNow = isAddonLoad && system_clock::now() >= system_clock::time_point(seconds(g_settings.nextResetUtcEpochSeconds));
            break;
        default: // daily/weekly resets can land at any moment the addon is running
            dueNow = system_clock::now() >= system_clock::time_point(seconds(g_settings.nextResetUtcEpochSeconds));
            break;
    }

    if (!dueNow) {
        return;
    }

    g_sessionTracker.Reset();
    SaveSessionState();
    RecomputeNextResetDateTime();
    SaveSettings();

    g_api->Log(LOGL_INFO, "DrfGoldTracker", "Automatic session reset triggered.");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReasonForCall, LPVOID /*lpReserved*/)
{
    if (ulReasonForCall == DLL_PROCESS_ATTACH) {
        g_hSelf = hModule;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
    // Randomly generated (crypto RNG, not derived from the name/author) -
    // Nexus has no central signature registry, so there's no way to
    // guarantee no collision. If this addon is ever published, cross-check
    // https://raidcore.gg/Addons first and regenerate if it happens to
    // collide with a listed one.
    g_addonDef.Signature = 0xC086A342;
    g_addonDef.APIVersion = NEXUS_API_VERSION;
    g_addonDef.Name = "DrfGoldTracker";
    g_addonDef.Version = { 0, 1, 0, 0 };
    g_addonDef.Author = "faker-ino";
    g_addonDef.Description = "Live gold/item farming tracker via DRF (drf.rs)";
    g_addonDef.Load = [](AddonAPI_t* aApi) {
        std::lock_guard<std::recursive_mutex> lifecycleLock(g_lifecycleMutex);
        if (g_lifecycleDepth > 0) {
            // Same-thread reentrant call - see g_lifecycleDepth's comment.
            if (aApi) {
                aApi->Log(LOGL_WARNING, "DrfGoldTracker", "Load() called reentrantly - ignoring.");
            }
            return;
        }
        struct DepthGuard {
            DepthGuard() { g_lifecycleDepth++; }
            ~DepthGuard() { g_lifecycleDepth--; }
        } depthGuard;

        if (g_loaded) {
            // Already loaded and no Unload() has run since - see g_loaded's
            // comment. Ignore rather than re-running startup on top of an
            // already-running addon.
            if (aApi) {
                aApi->Log(LOGL_WARNING, "DrfGoldTracker", "Load() called while already loaded - ignoring.");
            }
            return;
        }
        g_loaded = true;

        g_api = aApi;

        try {
            ImGui::SetCurrentContext((ImGuiContext*)g_api->ImguiContext);
            ImGui::SetAllocatorFunctions((void* (*)(size_t, void*))g_api->ImguiMalloc, (void(*)(void*, void*))g_api->ImguiFree);

            g_settingsPath = g_api->Paths_GetAddonDirectory("DrfGoldTracker/settings.json");
            g_settings = TrackerSettings::LoadFromFile(g_settingsPath);
            LoadSessionState(g_api->Paths_GetAddonDirectory("DrfGoldTracker/session.json"));
            LoadItemCache(g_api->Paths_GetAddonDirectory("DrfGoldTracker/item_cache.json"));
            strncpy_s(g_tokenBuf, g_settings.drfToken.c_str(), sizeof(g_tokenBuf) - 1);

            CheckAndApplyAutomaticReset(/*isAddonLoad*/ true);

            // ix::initNetSystem() (WSAStartup on Windows) only needs to run
            // once for the whole process, ever - guarded so that repeatedly
            // enabling/disabling the addon (which calls this Load lambda
            // again without necessarily reloading the DLL) can't pair up
            // mismatched init/cleanup calls against DrfClient's websocket and
            // Gw2Api's HTTP client while they might still be mid-request.
            static bool s_netSystemInitialized = false;
            if (!s_netSystemInitialized) {
                ix::initNetSystem();
                s_netSystemInitialized = true;
            }

            if (!g_settings.drfToken.empty()) {
                g_drfClient.SetToken(g_settings.drfToken);
            }

            // g_gw2Api is a process-lifetime static (this Load lambda can run
            // again without the DLL being reloaded) - its worker thread is
            // permanently stopped by Unload()'s Stop() call, so it must be
            // restarted here on every re-enable. No-op if already running.
            g_gw2Api.Start();

            // SetCacheDir() before Start() every time (not just once) -
            // cheap and keeps it correct even in the hypothetical case
            // Paths_GetAddonDirectory's answer ever changed between reloads.
            g_iconCache.SetCacheDir(g_api->Paths_GetAddonDirectory("DrfGoldTracker/icons"));
            g_iconCache.Start();

            StartAutosaveThread();

            g_api->GUI_Register(RT_Render, AddonRender);
            g_api->GUI_Register(RT_OptionsRender, AddonOptions);

            // An empty default keybind string here previously left Nexus's
            // QuickAccess tooltip showing a stray "(null)" instead of no
            // keybind at all - give it a real default combo instead.
            g_api->InputBinds_RegisterWithString(kToggleWindowKeybindId, ToggleWindowKeybindHandler, "ALT+SHIFT+G");
            // Icon textures are (re)created every frame in AddonRenderImpl
            // (GetOrCreateFromMemory is safe/cheap to call repeatedly until
            // it stops returning null) rather than once here, so QuickAccess
            // still ends up with a valid texture even if this Load() call
            // ran before ImGui/the renderer backend was fully ready.
            if (g_settings.showQuickAccessIcon) {
                g_api->QuickAccess_Add(kQuickAccessShortcutId, kQuickAccessIconId, kQuickAccessIconHoverId,
                    kToggleWindowKeybindId, "DRF Gold Tracker");
            }

            g_api->Log(LOGL_INFO, "DrfGoldTracker", "loaded.");
        } catch (const std::exception& e) {
            // An exception escaping this callback would crash the whole game
            // process, not just this addon - Nexus's caller has no idea how
            // to handle a C++ exception from a raw function pointer.
            g_api->Log(LOGL_CRITICAL, "DrfGoldTracker", (std::string("Load() failed: ") + e.what()).c_str());
        } catch (...) {
            g_api->Log(LOGL_CRITICAL, "DrfGoldTracker", "Load() failed: unknown exception");
        }
    };
    g_addonDef.Unload = []() {
        std::lock_guard<std::recursive_mutex> lifecycleLock(g_lifecycleMutex);
        if (g_lifecycleDepth > 0) {
            // Same-thread reentrant call - see g_lifecycleDepth's comment.
            // This is the exact shape of "pressed disable again while it
            // was still disabling": without this check, teardown below
            // would run a second time on top of an already-in-progress
            // teardown of the same non-reentrant-safe objects.
            if (g_api) {
                g_api->Log(LOGL_WARNING, "DrfGoldTracker", "Unload() called reentrantly - ignoring.");
            }
            return;
        }
        struct DepthGuard {
            DepthGuard() { g_lifecycleDepth++; }
            ~DepthGuard() { g_lifecycleDepth--; }
        } depthGuard;

        if (!g_loaded) {
            // Already unloaded (or never loaded) - see g_loaded's comment.
            // Ignore rather than re-running teardown against state a prior
            // Unload() call already tore down (double GUI_Deregister,
            // double Stop() on already-stopped worker threads, etc.) - this
            // is what a queue of several disable clicks landing as separate
            // calls, not a single nested one, actually looked like.
            if (g_api) {
                g_api->Log(LOGL_WARNING, "DrfGoldTracker", "Unload() called while already unloaded - ignoring.");
            }
            return;
        }
        g_loaded = false;

        try {
            g_api->GUI_Deregister(AddonRender);
            g_api->GUI_Deregister(AddonOptions);
            g_api->QuickAccess_Remove(kQuickAccessShortcutId);
            g_api->InputBinds_Deregister(kToggleWindowKeybindId);

            // Stopped before the final save below so the two can't race
            // writing session.json/item_cache.json against each other.
            StopAutosaveThread();

            if (g_settings.automaticReset == AutomaticReset::MinutesAfterUnload) {
                RecomputeNextResetDateTime(); // arms the countdown checked on the next Load()
            }
            SaveSettings();
            SaveSessionState();
            SaveItemCache();
        } catch (const std::exception& e) {
            g_api->Log(LOGL_CRITICAL, "DrfGoldTracker", (std::string("Unload() save step failed: ") + e.what()).c_str());
        } catch (...) {
            g_api->Log(LOGL_CRITICAL, "DrfGoldTracker", "Unload() save step failed: unknown exception");
        }

        try {
            // Both background threads must be fully joined before Nexus
            // calls FreeLibrary - otherwise a thread still running inside
            // this DLL blocks unload until the whole game process closes.
            // Net system is intentionally left initialized (see Load) so a
            // subsequent re-enable doesn't need to re-pair init/cleanup.
            g_drfClient.Stop();
            g_gw2Api.Stop();
            g_iconCache.Stop();

            g_api->Log(LOGL_INFO, "DrfGoldTracker", "unloaded.");
        } catch (const std::exception& e) {
            g_api->Log(LOGL_CRITICAL, "DrfGoldTracker", (std::string("Unload() failed: ") + e.what()).c_str());
        } catch (...) {
            g_api->Log(LOGL_CRITICAL, "DrfGoldTracker", "Unload() failed: unknown exception");
        }
    };
    g_addonDef.Flags = AF_None;
    g_addonDef.Provider = UP_None;
    g_addonDef.UpdateLink = nullptr;

    return &g_addonDef;
}

static const char* StatusLabel(DrfConnectionStatus status)
{
    switch (status) {
        case DrfConnectionStatus::Disconnected: return "Disconnected (no token)";
        case DrfConnectionStatus::Connecting:    return "Connecting...";
        case DrfConnectionStatus::Connected:     return "Connected";
        case DrfConnectionStatus::AuthFailed:    return "Authentication failed - check your DRF token";
    }
    return "Unknown";
}

static ImVec4 StatusColor(DrfConnectionStatus status)
{
    switch (status) {
        case DrfConnectionStatus::Connected:     return ImVec4(0.3f, 0.85f, 0.3f, 1.0f);
        case DrfConnectionStatus::Connecting:    return ImVec4(0.9f, 0.75f, 0.2f, 1.0f);
        case DrfConnectionStatus::AuthFailed:    return ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
        case DrfConnectionStatus::Disconnected:  return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    }
    return ImVec4(1, 1, 1, 1);
}

// Per-denomination coin textures (gold/silver/copper), baked into the DLL
// from resources/icons/{gold,silver,copper}_coin.png - the "highres" coin
// renders from wiki.guildwars2.com/wiki/Coin, embedded the same way as the
// QuickAccess icons (see cmake/EmbedIcon.cmake). There's no per-denomination
// icon URL in the GW2 API itself (the API's icon for currency id 1 "Coin" is
// a single combined wallet icon), so these can't be fetched through
// IconCache like item/currency icons are - they ship as static assets
// instead. Refreshed once per frame in AddonRenderImpl (GetOrCreateFromMemory
// is safe/cheap to call repeatedly until it stops returning null, same
// pattern as the QuickAccess icons). Used only by RenderCoinDenominationTile
// - the Currency section's Coin tile under TrackerSettings::iconOnlyView -
// not by RenderCoin, which keeps the plain "Ng Ss Cc" text format everywhere
// else (Profit/hour, Est. Value, tooltips, and Coin in table view).
static Texture_t* g_goldCoinTexture = nullptr;
static Texture_t* g_silverCoinTexture = nullptr;
static Texture_t* g_copperCoinTexture = nullptr;

// Formats copper into GW2's "Ng Ss Cc" style, with icon-free plain text
// colored the way the in-game wallet does (gold/silver/bronze). dimmed
// halves the alpha of all of it - used for items whose count has gone
// negative this session (used more than were farmed, e.g. consumables).
static void RenderCoin(int64_t copper, bool dimmed = false)
{
    const float alpha = dimmed ? 0.5f : 1.0f;
    const bool negative = copper < 0;
    uint64_t abs = static_cast<uint64_t>(negative ? -copper : copper);
    const uint64_t gold = abs / 10000;
    const uint64_t silver = (abs / 100) % 100;
    const uint64_t bronze = abs % 100;

    if (negative) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, alpha), "-");
        ImGui::SameLine(0, 0);
    }
    if (gold > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, alpha), "%llug ", (unsigned long long)gold);
        ImGui::SameLine(0, 0);
    }
    ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.78f, alpha), "%llus ", (unsigned long long)silver);
    ImGui::SameLine(0, 0);
    ImGui::TextColored(ImVec4(0.72f, 0.45f, 0.2f, alpha), "%lluc", (unsigned long long)bronze);
}

// Nexus's Textures_GetOrCreateFromFile caches the resulting texture by
// identifier (decode+GPU-upload only has to happen once), but that first
// call per identifier is real work - measured data showed a multi-ms
// frame-time burst exactly when several new items' icons became available
// in the same few frames (e.g. right after a batch of newly-discovered
// drops finish downloading). g_warmedIconIdentifiers tracks which
// identifiers this process has already asked Nexus to create, and
// g_texturesCreatedThisFrame/kMaxNewTexturesPerFrame budgets how many
// brand-new ones get created per frame, so a burst of newly-ready icons
// gets spread across several frames instead of hitching one of them.
static std::unordered_set<std::string> g_warmedIconIdentifiers;
static int g_texturesCreatedThisFrame = 0;
constexpr int kMaxNewTexturesPerFrame = 2;

// Goes through IconCache (disk-backed) rather than Nexus's own
// Textures_GetOrCreateFromURL (which re-downloads from render.guildwars2.com
// every game session) - once IconCache has the file on disk, this just asks
// Nexus to load a texture from that local file, which is nearly free on
// every subsequent frame since Nexus caches the resulting texture itself.
static Texture_t* GetOrRequestIconTexture(const std::string& iconUrl)
{
    if (iconUrl.empty()) {
        return nullptr;
    }
    if (!g_iconCache.IsCached(iconUrl)) {
        g_iconCache.RequestDownload(iconUrl);
        return nullptr; // not on disk yet - caller draws a placeholder this frame
    }
    const std::string localPath = g_iconCache.GetLocalPath(iconUrl);
    const std::string identifier = "DrfGoldTracker_Icon_" + localPath;

    const bool alreadyWarmed = g_warmedIconIdentifiers.count(identifier) != 0;
    if (!alreadyWarmed) {
        if (g_texturesCreatedThisFrame >= kMaxNewTexturesPerFrame) {
            return nullptr; // this frame's budget for brand-new textures is spent - try again next frame
        }
        g_texturesCreatedThisFrame++;
        g_warmedIconIdentifiers.insert(identifier);
    }
    return g_api->Textures_GetOrCreateFromFile(identifier.c_str(), localPath.c_str());
}

// Draws the icon (or a blank placeholder of the same size, so table columns
// stay aligned while it's still loading) followed by SameLine() so the
// caller can immediately draw the label next to it.
static void RenderIcon(const std::string& iconUrl)
{
    const float size = ImGui::GetTextLineHeight();
    Texture_t* texture = GetOrRequestIconTexture(iconUrl);
    if (texture && texture->Resource) {
        ImGui::Image((ImTextureID)texture->Resource, ImVec2(size, size));
    } else {
        ImGui::Dummy(ImVec2(size, size));
    }
    ImGui::SameLine();
}

// GW2's actual in-game rarity colors (from the item tooltip/wiki, not an
// approximation) - unknown/not-yet-loaded rarity gets a neutral grey.
static ImVec4 RarityColor(const std::string& rarity)
{
    if (rarity == "Junk")       return ImVec4(0.667f, 0.667f, 0.667f, 1.0f);
    if (rarity == "Basic")      return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    if (rarity == "Fine")       return ImVec4(0.384f, 0.643f, 0.855f, 1.0f);
    if (rarity == "Masterwork") return ImVec4(0.102f, 0.576f, 0.024f, 1.0f);
    if (rarity == "Rare")       return ImVec4(0.988f, 0.816f, 0.043f, 1.0f);
    if (rarity == "Exotic")     return ImVec4(1.0f, 0.643f, 0.020f, 1.0f);
    if (rarity == "Ascended")   return ImVec4(0.984f, 0.243f, 0.553f, 1.0f);
    if (rarity == "Legendary")  return ImVec4(0.298f, 0.075f, 0.616f, 1.0f);
    return ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
}

// Looks up the command line registered for the user's default browser
// (e.g. `"C:\...\firefox.exe" -osint -url "%1"`), the same way ShellExecute
// resolves "open a URL" internally - reimplemented here (rather than just
// calling ShellExecuteW) because CreateProcessW is the only way to control
// the child process's environment block (see LaunchBrowserWithCleanEnvironment's
// comment for why that matters), which ShellExecuteW/Ex don't expose.
// Returns an empty string if no per-user choice is registered (falls back
// to the HTTP ProgID's own default).
static std::wstring GetDefaultHttpHandlerCommand()
{
    std::wstring progId;
    HKEY userChoiceKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\http\\UserChoice",
            0, KEY_READ, &userChoiceKey) == ERROR_SUCCESS) {
        wchar_t buf[256] = {};
        DWORD size = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(userChoiceKey, L"ProgId", nullptr, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS && type == REG_SZ) {
            progId = buf;
        }
        RegCloseKey(userChoiceKey);
    }
    if (progId.empty()) {
        progId = L"http"; // fall back to HKCR\http\shell\open\command directly
    }

    std::wstring command;
    HKEY commandKey;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, (progId + L"\\shell\\open\\command").c_str(), 0, KEY_READ, &commandKey) == ERROR_SUCCESS) {
        wchar_t buf[1024] = {};
        DWORD size = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(commandKey, nullptr, nullptr, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            command = buf;
        }
        RegCloseKey(commandKey);
    }
    return command;
}

// Launches `url` in the default browser via CreateProcessW with a clean,
// non-redirected environment block, instead of just calling ShellExecuteW.
// Needed specifically for GW2Launcher users: GW2Launcher runs multiple
// simultaneous GW2 clients by redirecting each instance's %APPDATA%/
// %LOCALAPPDATA%/%USERPROFILE% to an isolated per-account folder. A browser
// spawned the normal way (ShellExecuteW, which inherits the calling
// process's environment) inherits that redirection too, so it resolves a
// DIFFERENT browser profile than the user's real, already-running one - and
// then can't recognize it as "the same" instance to hand a URL off to,
// which for Firefox specifically surfaced as an "already running, but not
// responding" dialog (root-caused via diagnostic logging - GW2Launcher was
// redirecting APPDATA/USERPROFILE to a per-account folder under
// %APPDATA%\Gw2Launcher\data\<n>). CreateEnvironmentBlock resolves the real
// per-user environment straight from the token, bypassing whatever the
// calling process's own (possibly redirected) environment variables
// currently say. CREATE_BREAKAWAY_FROM_JOB is also passed since GW2 is
// commonly run inside a Job Object (Steam wraps games in one); harmless if
// the job doesn't allow it or there isn't one. Returns false (caller falls
// back to plain ShellExecuteW) if the registry lookup or CreateProcessW
// fails for any reason - better a browser window opens with the wrong
// profile than not at all.
static bool LaunchBrowserWithCleanEnvironment(const std::wstring& url)
{
    const std::wstring commandTemplate = GetDefaultHttpHandlerCommand();
    if (commandTemplate.empty()) {
        return false;
    }

    const std::wstring quotedUrl = L"\"" + url + L"\"";
    std::wstring commandLine = commandTemplate;
    const size_t placeholder = commandLine.find(L"%1");
    if (placeholder != std::wstring::npos) {
        commandLine.replace(placeholder, 2, quotedUrl);
    } else {
        commandLine += L" " + quotedUrl;
    }

    // CreateProcessW requires a mutable buffer for lpCommandLine (it may
    // write a null terminator into it in place) - a std::wstring's internal
    // buffer isn't guaranteed mutable-safe for that even via &str[0].
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    LPVOID environmentBlock = nullptr;
    HANDLE processToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &processToken)) {
        CreateEnvironmentBlock(&environmentBlock, processToken, FALSE);
        CloseHandle(processToken);
    }

    STARTUPINFOW startupInfo = { sizeof(startupInfo) };
    PROCESS_INFORMATION processInfo = {};
    const BOOL ok = CreateProcessW(nullptr, mutableCommandLine.data(), nullptr, nullptr, FALSE,
        CREATE_BREAKAWAY_FROM_JOB | (environmentBlock ? CREATE_UNICODE_ENVIRONMENT : 0),
        environmentBlock, nullptr, &startupInfo, &processInfo);

    if (environmentBlock) {
        DestroyEnvironmentBlock(environmentBlock);
    }
    if (!ok) {
        return false;
    }
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}

// Opens `url` in the default browser, preferring LaunchBrowserWithCleanEnvironment
// (see its comment) and falling back to plain ShellExecuteW if that fails
// for any reason. Runs on a dedicated, detached thread with its own STA COM
// apartment, since the ShellExecuteW fallback needs COM initialized and
// this keeps both paths off the render thread.
static void OpenUrlInBrowser(const std::string& url)
{
    std::thread([url]() {
        const bool comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        std::wstring wideUrl(url.begin(), url.end()); // url is pure ASCII (percent-encoded) - safe narrow->wide widen
        if (!LaunchBrowserWithCleanEnvironment(wideUrl)) {
            ShellExecuteW(nullptr, L"open", wideUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (comInitialized) {
            CoUninitialize();
        }
    }).detach();
}

// Opens the GW2 wiki page for `itemName` in the default browser, via the
// wiki's own search-with-redirect endpoint ("go") rather than trying to
// build the exact article URL ourselves - MediaWiki normalizes titles
// (spaces to underscores, capitalization, apostrophes/quotes in names like
// "Zhaitan's Reach") in ways that are simpler to let it handle than to
// replicate, and an exact-title search redirects straight to the matching
// page.
static void OpenGw2WikiPage(const std::string& itemName)
{
    std::ostringstream encoded;
    for (unsigned char c : itemName) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << (char)c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            encoded << buf;
        }
    }
    std::string url = "https://wiki.guildwars2.com/index.php?title=Special:Search&search=" + encoded.str() + "&go=Go";
    OpenUrlInBrowser(url);
}

// Item-specific icon: adds a colored border for rarity (items only -
// currencies have no rarity) and, when dimmed, halves the icon's opacity to
// visually flag a stat whose count has gone negative this session. No
// border for Junk/Basic (grey/white) - those aren't worth calling out, only
// Fine (blue) and rarer get one.
static void RenderItemIcon(const Gw2ItemInfo& info, bool dimmed)
{
    const float size = ImGui::GetTextLineHeight();
    Texture_t* texture = GetOrRequestIconTexture(info.iconUrl);
    if (texture && texture->Resource) {
        const ImVec2 topLeft = ImGui::GetCursorScreenPos();
        const ImVec4 tint = dimmed ? ImVec4(1.0f, 1.0f, 1.0f, 0.4f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        ImGui::Image((ImTextureID)texture->Resource, ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1), tint);
        if (info.rarity != "Junk" && info.rarity != "Basic" && !info.rarity.empty()) {
            const ImVec4 rarityColor = RarityColor(info.rarity);
            ImGui::GetWindowDrawList()->AddRect(
                topLeft, ImVec2(topLeft.x + size, topLeft.y + size),
                ImGui::ColorConvertFloat4ToU32(ImVec4(rarityColor.x, rarityColor.y, rarityColor.z, dimmed ? 0.5f : 1.0f)),
                0.0f, 0, 1.5f);
        }
    } else {
        ImGui::Dummy(ImVec2(size, size));
    }
    ImGui::SameLine();
}

struct ItemRow {
    int64_t id;
    int64_t count;
    Gw2ItemInfo info;
    SellClassification sellClass;
    int64_t unitValue;       // EstimateItemUnitValueInCopper(info)
    int64_t estimatedValue;  // count * unitValue
};

// --- Search filter -----------------------------------------------------
// Session-only UI state (not persisted to disk, not part of TrackerSettings)
// for the search box next to Reset Session. Deliberately separate from
// PassesItemFilters/PassesCurrencyFilters below: those are skipped for
// Favorites via applyDisplayFilters (a pinned stat shouldn't vanish because
// of a display filter), but search is meant to narrow all three sections -
// Favorites, Currencies, Items - uniformly, so every render call site checks
// MatchesSearchQuery unconditionally instead of folding it into that gate.
static char s_searchBuf[128] = "";
static bool s_searchBoxOpen = false;
static bool s_focusSearchBox = false;

static bool MatchesSearchQuery(const std::string& name, int64_t id, const char* fallbackPrefix)
{
    if (s_searchBuf[0] == '\0') {
        return true;
    }
    std::string haystack = (!name.empty()) ? name : (std::string(fallbackPrefix) + " #" + std::to_string(id));
    std::string needle = s_searchBuf;
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return haystack.find(needle) != std::string::npos;
}

// Filters are exclude-lists: a stat matching any checked "hide" flag is
// hidden, independent of the others (see TrackerSettings' doc comment).
static bool PassesItemFilters(const Gw2ItemInfo& info, const SellClassification& sellClass, int64_t count)
{
    const auto& s = g_settings;

    // canSellToVendor and canSellOnTp aren't mutually exclusive - an item
    // can be sellable both ways. "Hide vendor-sellable" only makes sense as
    // "hide vendor-ONLY junk", so it must not also catch a dual-sellable
    // item just because it happens to have a vendor value too; that item
    // still belongs to "TP-sellable", which does hide unconditionally on
    // any TP-sellable item, dual or not.
    if (s.filterHideVendorSellable && sellClass.canSellToVendor && !sellClass.canSellOnTp) return false;
    if (s.filterHideTpSellable && sellClass.canSellOnTp) return false;
    if (s.filterHideNotSellable && sellClass.canNotBeSold) return false;

    if (s.filterHidePositiveCount && count > 0) return false;
    if (s.filterHideNegativeCount && count < 0) return false;

    if (s.filterHideKnownByApi && info.loaded && !info.name.empty()) return false;
    if (s.filterHideUnknownByApi && info.loaded && info.name.empty()) return false;

    if (!s.filterHiddenRarities.empty() && s.filterHiddenRarities.count(info.rarity)) return false;

    return true;
}

// Currencies have no vendor/TP price data in this addon, so only the
// count-sign and known-by-API filters apply to them.
static bool PassesCurrencyFilters(const Gw2CurrencyInfo& info, int64_t count)
{
    const auto& s = g_settings;

    if (s.filterHidePositiveCount && count > 0) return false;
    if (s.filterHideNegativeCount && count < 0) return false;

    if (s.filterHideKnownByApi && info.loaded && !info.name.empty()) return false;
    if (s.filterHideUnknownByApi && info.loaded && info.name.empty()) return false;

    return true;
}

static void SortItemRows(std::vector<ItemRow>& rows)
{
    const bool descending = g_settings.sortDescending;

    std::sort(rows.begin(), rows.end(), [descending](const ItemRow& a, const ItemRow& b) {
        if (g_settings.sortKey == SortKey::Name) {
            if (a.info.name != b.info.name) {
                return descending ? (a.info.name > b.info.name) : (a.info.name < b.info.name);
            }
        } else {
            // SortKey::Count or SortKey::ProfitTotal - both plain int64_t compares.
            const int64_t left = g_settings.sortKey == SortKey::Count ? a.count : a.estimatedValue;
            const int64_t right = g_settings.sortKey == SortKey::Count ? b.count : b.estimatedValue;
            if (left != right) {
                if (left < 0 && right < 0) {
                    // Both dimmed "net loss" rows: order by magnitude rather
                    // than raw value, so descending still reads as "biggest
                    // first" for losses (e.g. -18 above -1) instead of -1
                    // outranking -18 just because it's numerically larger.
                    return descending ? (left < right) : (left > right);
                }
                return descending ? (left > right) : (left < right);
            }
            // Tied on Est. Value: break by count in the same direction, so
            // sorting by value descending also surfaces the highest count
            // within a tied tier first (and lowest count first when
            // ascending), instead of falling through to the sign-only
            // tiebreak below.
            if (g_settings.sortKey == SortKey::ProfitTotal && a.count != b.count) {
                if (a.count < 0 && b.count < 0) {
                    return descending ? (a.count < b.count) : (a.count > b.count);
                }
                return descending ? (a.count > b.count) : (a.count < b.count);
            }
        }
        // Tiebreak: two rows can easily land on the same Est. Value (most
        // commonly 0, e.g. an unsellable bound reward item) without having
        // anything else in common. Sorting those by count sign keeps "still
        // have some of it" (positive) visually separate from "used more
        // than was found" (negative) instead of an arbitrary interleaving.
        const int leftSign = a.count > 0 ? 1 : (a.count < 0 ? -1 : 0);
        const int rightSign = b.count > 0 ? 1 : (b.count < 0 ? -1 : 0);
        return descending ? (leftSign > rightSign) : (leftSign < rightSign);
    });
}

// Builds ItemRow entries for every id in itemTotals that includeId()
// accepts (count != 0 always required). applyDisplayFilters controls
// whether the Filtering section's settings apply - the Favorites list
// ignores them (a pinned stat should never disappear because of a filter).
static std::vector<ItemRow> BuildItemRows(
    const std::unordered_map<int64_t, int64_t>& itemTotals,
    const std::function<bool(int64_t)>& includeId,
    bool applyDisplayFilters)
{
    std::vector<ItemRow> rows;
    rows.reserve(itemTotals.size());
    for (const auto& [id, count] : itemTotals) {
        if (count == 0 || !includeId(id)) continue;

        Gw2ItemInfo info = g_gw2Api.GetItemInfo(id);
        SellClassification sellClass = ClassifyItemSellMethod(info);
        if (applyDisplayFilters && !PassesItemFilters(info, sellClass, count)) continue;

        const int64_t unitValue = EstimateItemUnitValueInCopper(info, g_settings.estValueUseInstantSellPrice);
        // move, not copy - info was just built fresh above and isn't read
        // again, so there's no reason to pay for a second copy of its three
        // strings on top of the one GetItemInfo() already made.
        rows.push_back({ id, count, std::move(info), sellClass, unitValue, unitValue * count });
    }
    return rows;
}

// ImGuiTableColumnFlags for a sortable column: marks it as the table's
// current sort column (with the right arrow direction) if it matches
// g_settings.sortKey/sortDescending, so the header reflects our persisted
// setting instead of resetting to ImGui's own default every time the table
// is rebuilt (e.g. after a filter/tab change recreates it).
static ImGuiTableColumnFlags SortColumnFlags(SortKey key)
{
    if (g_settings.sortKey != key) {
        return ImGuiTableColumnFlags_None;
    }
    return ImGuiTableColumnFlags_DefaultSort
        | (g_settings.sortDescending ? ImGuiTableColumnFlags_PreferSortDescending : ImGuiTableColumnFlags_None);
}

// Reads ImGui's own click-to-sort state for the table just set up (Item /
// Count / Est. Value, in that column order) and mirrors a change into
// g_settings so SortItemRows() picks it up next frame. No-op unless the
// user actually clicked a header this frame.
static void SyncSortFromTableHeaderClick()
{
    ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
    if (!sortSpecs || !sortSpecs->SpecsDirty || sortSpecs->SpecsCount == 0) {
        return;
    }
    const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
    switch (spec.ColumnIndex) {
        case 0: g_settings.sortKey = SortKey::Name; break;
        case 1: g_settings.sortKey = SortKey::Count; break;
        case 2: g_settings.sortKey = SortKey::ProfitTotal; break;
        default: break;
    }
    g_settings.sortDescending = spec.SortDirection == ImGuiSortDirection_Descending;
    SaveSettings();
    sortSpecs->SpecsDirty = false;
}

// Rich hover tooltip for an item row/tile - name (colored by rarity) plus
// TP sell/buy and vendor prices. Shown in both the table view and the
// icon-grid view (see TrackerSettings::iconOnlyView) - assumes the caller's
// last ImGui item is the one that should trigger it (a Selectable or an
// InvisibleButton tile).
static void RenderItemTooltip(const ItemRow& row)
{
    if (!ImGui::IsItemHovered()) {
        return;
    }
    // Nexus's shared ImGui theme can leave WindowBorderSize at 0, so the
    // tooltip would otherwise blend into whatever's behind it - force a thin
    // border just for this window rather than relying on the ambient style.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::BeginTooltip();
    const std::string name = (row.info.loaded && !row.info.name.empty())
        ? row.info.name : ("Item #" + std::to_string(row.id));
    ImGui::TextColored(RarityColor(row.info.rarity), "%s", name.c_str());

    // TP prices are shown fee-adjusted (what selling would actually pay),
    // matching the "already deducted" note below - the raw values in
    // Gw2ItemInfo are pre-fee listing/order prices (see its doc comment).
    bool anyTpPrice = false;
    if (row.sellClass.canSellOnTp) {
        if (row.info.tpSellPriceInCopper >= 0) {
            ImGui::TextUnformatted("TP Sell:");
            ImGui::SameLine();
            RenderCoin(row.info.tpSellPriceInCopper * TP_FEE_NUMERATOR / TP_FEE_DENOMINATOR);
            anyTpPrice = true;
        }
        if (row.info.tpBuyPriceInCopper >= 0) {
            ImGui::TextUnformatted("TP Buy:");
            ImGui::SameLine();
            RenderCoin(row.info.tpBuyPriceInCopper * TP_FEE_NUMERATOR / TP_FEE_DENOMINATOR);
            anyTpPrice = true;
        }
    }
    bool anyVendorPrice = false;
    if (row.sellClass.canSellToVendor) {
        ImGui::TextUnformatted("Vendor:");
        ImGui::SameLine();
        RenderCoin(row.info.vendorValue);
        anyVendorPrice = true;
    }
    if (anyTpPrice) {
        ImGui::TextDisabled("(15%% fee applied to tp sell/buy)");
    }
    if (!anyTpPrice && !anyVendorPrice) {
        ImGui::TextDisabled("Not sellable");
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Right click for more options.");
    ImGui::EndTooltip();
    ImGui::PopStyleVar();
}

// Simple hover tooltip for a currency row/tile - currencies carry no TP/
// vendor price data in this addon, so just the name plus the same
// right-click hint as items.
static void RenderCurrencyTooltip(const std::string& label)
{
    if (!ImGui::IsItemHovered()) {
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f); // see RenderItemTooltip's comment
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(label.c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("Right click for more options.");
    ImGui::EndTooltip();
    ImGui::PopStyleVar();
}

// Shared table body for any list of ItemRow (main Items list or Favorites).
// Right-click gives Favorite/Unfavorite (toggling based on current state)
// and Ignore - the two are mutually exclusive (see TrackerSettings' doc
// comment), so picking one clears the id from the other set. enableSort
// wires the headers up to g_settings.sortKey/sortDescending (via
// SyncSortFromTableHeaderClick) - off for Favorites, which keeps its own
// fixed alphabetical order regardless of how the main Items list is sorted.
static void RenderItemRows(const char* tableId, const std::vector<ItemRow>& rows, bool enableSort)
{
    // NoBordersInBody keeps the Resizable column-border hit-test confined to
    // the header row (see ImGui's TableUpdateBorders) - without it, the
    // invisible resize-grab strip runs down the full row list and randomly
    // steals mouse-wheel hover from whatever column boundary the cursor
    // happens to be near while scrolling.
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_NoBordersInBody;
    if (enableSort) {
        flags |= ImGuiTableFlags_Sortable;
    }
    if (!ImGui::BeginTable(tableId, 3, flags)) {
        return;
    }
    ImGui::TableSetupColumn("Item", enableSort ? SortColumnFlags(SortKey::Name) : ImGuiTableColumnFlags_None);
    ImGui::TableSetupColumn("Count", enableSort ? SortColumnFlags(SortKey::Count) : ImGuiTableColumnFlags_None);
    ImGui::TableSetupColumn("Est. Value", enableSort ? SortColumnFlags(SortKey::ProfitTotal) : ImGuiTableColumnFlags_None);
    ImGui::TableHeadersRow();
    if (enableSort) {
        SyncSortFromTableHeaderClick();
    }

    // Search narrows which rows the clipper below sees. Building a small
    // index list (not a copy of the ItemRows themselves, which each carry
    // three strings) keeps this cheap, and only happens at all while a query
    // is actually typed.
    const bool searching = s_searchBuf[0] != '\0';
    std::vector<int> filteredIndices;
    if (searching) {
        filteredIndices.reserve(rows.size());
        for (int i = 0; i < (int)rows.size(); ++i) {
            if (MatchesSearchQuery(rows[i].info.name, rows[i].id, "Item")) {
                filteredIndices.push_back(i);
            }
        }
    }
    const int rowCount = searching ? (int)filteredIndices.size() : (int)rows.size();

    // Clipped rather than a plain range-for: without it, every row - including
    // the hundreds scrolled off-screen once a session's item list grows over
    // many restarts - pays IsCached()'s filesystem::exists() stat call and a
    // Textures_GetOrCreateFromFile lookup every single frame, which is what
    // made the mouse wheel feel laggy on a long-running session. The clipper
    // keeps that cost bounded to the rows actually on screen. Safe here since
    // every row is a single, fixed-height line (no wrapped text).
    ImGuiListClipper clipper;
    clipper.Begin(rowCount);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const ItemRow& row = rows[searching ? filteredIndices[i] : i];
            // A count that's gone negative means this session used more of
            // the item than it dropped (e.g. a consumable) - flagged with a
            // dimmed icon and text, so it reads as "net loss" at a glance.
            const bool dimmed = row.count < 0;
            const bool isFavorite = g_settings.favoriteItemIds.count(row.id) != 0;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string itemLabel = (row.info.loaded && !row.info.name.empty())
                ? row.info.name : ("Item #" + std::to_string(row.id));
            RenderItemIcon(row.info, dimmed);
            if (dimmed) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
            // A row-spanning Selectable (rather than plain text) gives
            // BeginPopupContextItem() something to right-click.
            ImGui::Selectable((itemLabel + "##item" + std::to_string(row.id)).c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
            RenderItemTooltip(row);
            if (ImGui::BeginPopupContextItem()) {
                if (isFavorite) {
                    if (ImGui::MenuItem("Unfavorite")) {
                        g_settings.favoriteItemIds.erase(row.id);
                        SaveSettings();
                    }
                } else if (ImGui::MenuItem("Favorite")) {
                    g_settings.favoriteItemIds.insert(row.id);
                    g_settings.ignoredItemIds.erase(row.id);
                    SaveSettings();
                }
                if (ImGui::MenuItem("Ignore")) {
                    g_settings.ignoredItemIds.insert(row.id);
                    g_settings.favoriteItemIds.erase(row.id);
                    SaveSettings();
                }
                if (row.info.loaded && !row.info.name.empty() && ImGui::MenuItem("Open Wiki")) {
                    OpenGw2WikiPage(row.info.name);
                }
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            ImGui::Text("%lld", (long long)row.count);
            if (dimmed) ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            if (row.info.loaded) {
                RenderCoin(row.estimatedValue, dimmed);
            } else {
                ImGui::TextDisabled("...");
            }
        }
    }
    clipper.End();
    ImGui::EndTable();
}

// Shared table body for currencies (main "Currencies" list or
// Favorites) - same Favorite/Ignore right-click as RenderItemRows.
static void RenderCurrencyRows(
    const char* tableId,
    const std::unordered_map<int64_t, int64_t>& currencyTotals,
    const std::function<bool(int64_t)>& includeId,
    bool applyDisplayFilters)
{
    if (!ImGui::BeginTable(tableId, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_NoBordersInBody)) {
        return;
    }
    ImGui::TableSetupColumn("Currency");
    ImGui::TableSetupColumn("Count");
    ImGui::TableHeadersRow();

    // Sorted by currency (API) id ascending, always - not user-configurable
    // like the Items list. Sorting by count/name instead would move Coin
    // around, which looks broken since it's rendered as a split
    // gold/silver/copper trio rather than a plain number.
    std::vector<int64_t> sortedIds;
    sortedIds.reserve(currencyTotals.size());
    for (const auto& [id, count] : currencyTotals) sortedIds.push_back(id);
    std::sort(sortedIds.begin(), sortedIds.end());

    for (int64_t id : sortedIds) {
        // Coin ("raw gold") is included here like any other currency - it's
        // the running wallet delta DRF reports (gains AND spends, e.g. a
        // salvage kit's cost), not just what the top "Profit" line shows,
        // which folds it together with estimated item sell value.
        const int64_t count = currencyTotals.at(id);
        if (count == 0 || !includeId(id)) continue;

        Gw2CurrencyInfo info = g_gw2Api.GetCurrencyInfo(id);
        if (applyDisplayFilters && !PassesCurrencyFilters(info, count)) continue;
        if (!MatchesSearchQuery(info.name, id, "Currency")) continue;

        const bool dimmed = count < 0;
        const bool isFavorite = g_settings.favoriteCurrencyIds.count(id) != 0;

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const std::string currencyLabel = (info.loaded && !info.name.empty())
            ? info.name : ("Currency #" + std::to_string(id));
        RenderIcon(info.iconUrl);
        if (dimmed) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
        ImGui::Selectable((currencyLabel + "##curr" + std::to_string(id)).c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
        RenderCurrencyTooltip(currencyLabel);
        if (ImGui::BeginPopupContextItem()) {
            if (isFavorite) {
                if (ImGui::MenuItem("Unfavorite")) {
                    g_settings.favoriteCurrencyIds.erase(id);
                    SaveSettings();
                }
            } else if (ImGui::MenuItem("Favorite")) {
                g_settings.favoriteCurrencyIds.insert(id);
                g_settings.ignoredCurrencyIds.erase(id);
                SaveSettings();
            }
            if (ImGui::MenuItem("Ignore")) {
                g_settings.ignoredCurrencyIds.insert(id);
                g_settings.favoriteCurrencyIds.erase(id);
                SaveSettings();
            }
            ImGui::EndPopup();
        }
        ImGui::TableNextColumn();
        if (id == SessionTracker::COIN_CURRENCY_ID) {
            RenderCoin(count, dimmed); // "Ng Ss Cc" is far more readable than a bare copper integer
        } else {
            ImGui::Text("%lld", (long long)count);
        }
        if (dimmed) ImGui::PopStyleColor();
    }
    ImGui::EndTable();
}

// --- Icon-only grid view (TrackerSettings::iconOnlyView) -------------------
// A compact alternative to RenderItemRows/RenderCurrencyRows that lays out
// square icon tiles wrapped across the window width instead of table rows.
// Each tile shows just the icon plus a count badge; the name/price detail
// that a table row would otherwise show as text lives in the hover tooltip
// instead (see RenderItemTooltip/RenderCurrencyTooltip, used in both view
// modes).

// Bounds/step for the +/- icon-size control rendered next to the grid (see
// AddonRenderImpl) - clamps TrackerSettings::iconGridTileScale so repeated
// clicks can't shrink tiles down to unreadable slivers or blow up past a
// reasonable size.
constexpr float kMinIconGridTileScale = 1.2f;
constexpr float kMaxIconGridTileScale = 5.0f;
constexpr float kIconGridTileScaleStep = 0.3f;

static float GridTileSize()
{
    return ImGui::GetFontSize() * g_settings.iconGridTileScale;
}
constexpr float kGridTileSpacing = 6.0f;

// Draws `text` in a tile's bottom-left corner with a black outline so it
// stays legible over arbitrary icon artwork - the same treatment GW2's own
// inventory grid uses for stack-size numbers.
static void DrawGridBadge(ImVec2 tileMin, ImVec2 tileMax, const char* text, ImVec4 color)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const ImVec2 pos(tileMin.x + 2.0f, tileMax.y - textSize.y - 2.0f);
    const ImU32 outline = IM_COL32(0, 0, 0, 220);
    const ImU32 fill = ImGui::ColorConvertFloat4ToU32(color);
    for (const ImVec2& offset : { ImVec2(-1, 0), ImVec2(1, 0), ImVec2(0, -1), ImVec2(0, 1) }) {
        drawList->AddText(ImVec2(pos.x + offset.x, pos.y + offset.y), outline, text);
    }
    drawList->AddText(pos, fill, text);
}

// Grid version of RenderItemRows - same Favorite/Ignore right-click and
// hover tooltip, laid out as wrapped icon tiles instead of table rows.
static void RenderItemIconGrid(const char* gridId, const std::vector<ItemRow>& rows)
{
    ImGui::PushID(gridId);

    const bool searching = s_searchBuf[0] != '\0';
    std::vector<int> filteredIndices;
    if (searching) {
        filteredIndices.reserve(rows.size());
        for (int i = 0; i < (int)rows.size(); ++i) {
            if (MatchesSearchQuery(rows[i].info.name, rows[i].id, "Item")) {
                filteredIndices.push_back(i);
            }
        }
    }
    const size_t tileCount = searching ? filteredIndices.size() : rows.size();
    if (tileCount == 0) {
        ImGui::TextDisabled("(none)");
        ImGui::PopID();
        return;
    }

    const float tileSize = GridTileSize();
    const float windowVisibleX2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

    for (size_t i = 0; i < tileCount; ++i) {
        const ItemRow& row = rows[searching ? filteredIndices[i] : i];
        const bool dimmed = row.count < 0;
        const bool isFavorite = g_settings.favoriteItemIds.count(row.id) != 0;

        ImGui::PushID((int)row.id);
        ImGui::BeginGroup();

        const ImVec2 tileMin = ImGui::GetCursorScreenPos();
        const ImVec2 tileMax(tileMin.x + tileSize, tileMin.y + tileSize);
        Texture_t* texture = GetOrRequestIconTexture(row.info.iconUrl);
        ImGui::InvisibleButton("##tile", ImVec2(tileSize, tileSize));

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        if (texture && texture->Resource) {
            const ImU32 tint = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, dimmed ? 0.4f : 1.0f));
            drawList->AddImage((ImTextureID)texture->Resource, tileMin, tileMax, ImVec2(0, 0), ImVec2(1, 1), tint);
        } else {
            drawList->AddRectFilled(tileMin, tileMax, IM_COL32(40, 40, 40, 180));
        }
        if (row.info.rarity != "Junk" && row.info.rarity != "Basic" && !row.info.rarity.empty()) {
            const ImVec4 rarityColor = RarityColor(row.info.rarity);
            drawList->AddRect(tileMin, tileMax,
                ImGui::ColorConvertFloat4ToU32(ImVec4(rarityColor.x, rarityColor.y, rarityColor.z, dimmed ? 0.5f : 1.0f)),
                0.0f, 0, 1.5f);
        } else {
            drawList->AddRect(tileMin, tileMax, IM_COL32(80, 80, 80, 180));
        }

        const std::string countText = std::to_string(row.count);
        DrawGridBadge(tileMin, tileMax, countText.c_str(), dimmed ? ImVec4(0.75f, 0.75f, 0.75f, 1.0f) : ImVec4(1, 1, 1, 1));

        RenderItemTooltip(row);
        if (ImGui::BeginPopupContextItem("##ctx")) {
            if (isFavorite) {
                if (ImGui::MenuItem("Unfavorite")) {
                    g_settings.favoriteItemIds.erase(row.id);
                    SaveSettings();
                }
            } else if (ImGui::MenuItem("Favorite")) {
                g_settings.favoriteItemIds.insert(row.id);
                g_settings.ignoredItemIds.erase(row.id);
                SaveSettings();
            }
            if (ImGui::MenuItem("Ignore")) {
                g_settings.ignoredItemIds.insert(row.id);
                g_settings.favoriteItemIds.erase(row.id);
                SaveSettings();
            }
            if (row.info.loaded && !row.info.name.empty() && ImGui::MenuItem("Open Wiki")) {
                OpenGw2WikiPage(row.info.name);
            }
            ImGui::EndPopup();
        }

        ImGui::EndGroup();
        ImGui::PopID();

        if (i + 1 < tileCount) {
            const float nextTileX2 = ImGui::GetItemRectMax().x + kGridTileSpacing + tileSize;
            if (nextTileX2 < windowVisibleX2) {
                ImGui::SameLine(0.0f, kGridTileSpacing);
            }
        }
    }
    ImGui::PopID();
}

// One currency's icon tile (grid mode) - everything but Coin, which is
// special-cased into RenderCoinDenominationTile below.
static void RenderCurrencyGridTile(int64_t id, int64_t count, bool dimmed, const std::string& label, const std::string& iconUrl)
{
    const float tileSize = GridTileSize();
    const ImVec2 tileMin = ImGui::GetCursorScreenPos();
    const ImVec2 tileMax(tileMin.x + tileSize, tileMin.y + tileSize);

    ImGui::BeginGroup();
    Texture_t* texture = GetOrRequestIconTexture(iconUrl);
    ImGui::InvisibleButton("##tile", ImVec2(tileSize, tileSize));

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (texture && texture->Resource) {
        const ImU32 tint = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, dimmed ? 0.4f : 1.0f));
        drawList->AddImage((ImTextureID)texture->Resource, tileMin, tileMax, ImVec2(0, 0), ImVec2(1, 1), tint);
    } else {
        drawList->AddRectFilled(tileMin, tileMax, IM_COL32(40, 40, 40, 180));
    }
    drawList->AddRect(tileMin, tileMax, IM_COL32(80, 80, 80, 180));

    const std::string countText = std::to_string(count);
    DrawGridBadge(tileMin, tileMax, countText.c_str(), dimmed ? ImVec4(0.75f, 0.75f, 0.75f, 1.0f) : ImVec4(1, 1, 1, 1));

    RenderCurrencyTooltip(label);
    const bool isFavorite = g_settings.favoriteCurrencyIds.count(id) != 0;
    if (ImGui::BeginPopupContextItem("##ctx")) {
        if (isFavorite) {
            if (ImGui::MenuItem("Unfavorite")) {
                g_settings.favoriteCurrencyIds.erase(id);
                SaveSettings();
            }
        } else if (ImGui::MenuItem("Favorite")) {
            g_settings.favoriteCurrencyIds.insert(id);
            g_settings.ignoredCurrencyIds.erase(id);
            SaveSettings();
        }
        if (ImGui::MenuItem("Ignore")) {
            g_settings.ignoredCurrencyIds.insert(id);
            g_settings.favoriteCurrencyIds.erase(id);
            SaveSettings();
        }
        ImGui::EndPopup();
    }
    ImGui::EndGroup();
}

// Coin has no single icon to show in a tile (the GW2 API's icon for
// currency id 1 "Coin" is one combined wallet icon, not three separate
// per-denomination ones - see the embedded coin textures' comment above
// g_goldCoinTexture for why that rules out fetching real coin icons through
// IconCache like other currencies). Grid mode instead gives gold/silver/
// copper their own tile each, using the same embedded coin
// textures as RenderCoin - all three tied to the same underlying Coin
// currency id, so Favorite/Ignore on any one of them acts on Coin as a
// whole, same as it would from a single "Coin" row in table mode.
static void RenderCoinDenominationTile(Texture_t* texture, uint64_t amount, bool dimmed)
{
    const float tileSize = GridTileSize();
    const ImVec2 tileMin = ImGui::GetCursorScreenPos();
    const ImVec2 tileMax(tileMin.x + tileSize, tileMin.y + tileSize);
    const float alpha = dimmed ? 0.5f : 1.0f;

    ImGui::BeginGroup();
    ImGui::InvisibleButton("##tile", ImVec2(tileSize, tileSize));

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (texture && texture->Resource) {
        const ImU32 tint = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, alpha));
        drawList->AddImage((ImTextureID)texture->Resource, tileMin, tileMax, ImVec2(0, 0), ImVec2(1, 1), tint);
    } else {
        drawList->AddRectFilled(tileMin, tileMax, IM_COL32(40, 40, 40, 180));
    }
    drawList->AddRect(tileMin, tileMax, IM_COL32(80, 80, 80, 180));

    const std::string countText = std::to_string(amount);
    DrawGridBadge(tileMin, tileMax, countText.c_str(), dimmed ? ImVec4(0.75f, 0.75f, 0.75f, 1.0f) : ImVec4(1, 1, 1, 1));

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Coin\nRight click for more options.");
    }
    const bool isFavorite = g_settings.favoriteCurrencyIds.count(SessionTracker::COIN_CURRENCY_ID) != 0;
    if (ImGui::BeginPopupContextItem("##ctx")) {
        if (isFavorite) {
            if (ImGui::MenuItem("Unfavorite")) {
                g_settings.favoriteCurrencyIds.erase(SessionTracker::COIN_CURRENCY_ID);
                SaveSettings();
            }
        } else if (ImGui::MenuItem("Favorite")) {
            g_settings.favoriteCurrencyIds.insert(SessionTracker::COIN_CURRENCY_ID);
            g_settings.ignoredCurrencyIds.erase(SessionTracker::COIN_CURRENCY_ID);
            SaveSettings();
        }
        if (ImGui::MenuItem("Ignore")) {
            g_settings.ignoredCurrencyIds.insert(SessionTracker::COIN_CURRENCY_ID);
            g_settings.favoriteCurrencyIds.erase(SessionTracker::COIN_CURRENCY_ID);
            SaveSettings();
        }
        ImGui::EndPopup();
    }
    ImGui::EndGroup();
}

// Splits a Coin delta into up to three tile-drawing closures (gold/silver/
// copper), appended to `tiles` for RenderCurrencyIconGrid's uniform
// line-wrapping pass. Silver and copper are always included (even at 0),
// matching the in-game wallet's convention of always showing all three;
// gold is only included when nonzero, same as RenderCoin.
static void AppendCoinTileDrawers(std::vector<std::function<void()>>& tiles, int64_t copper)
{
    const bool dimmed = copper < 0;
    const uint64_t abs = static_cast<uint64_t>(dimmed ? -copper : copper);
    const uint64_t gold = abs / 10000;
    const uint64_t silver = (abs / 100) % 100;
    const uint64_t bronze = abs % 100;

    if (gold > 0) {
        tiles.push_back([gold, dimmed]() { RenderCoinDenominationTile(g_goldCoinTexture, gold, dimmed); });
    }
    tiles.push_back([silver, dimmed]() { RenderCoinDenominationTile(g_silverCoinTexture, silver, dimmed); });
    tiles.push_back([bronze, dimmed]() { RenderCoinDenominationTile(g_copperCoinTexture, bronze, dimmed); });
}

// Grid version of RenderCurrencyRows.
static void RenderCurrencyIconGrid(
    const char* gridId,
    const std::unordered_map<int64_t, int64_t>& currencyTotals,
    const std::function<bool(int64_t)>& includeId,
    bool applyDisplayFilters)
{
    ImGui::PushID(gridId);

    std::vector<int64_t> sortedIds;
    sortedIds.reserve(currencyTotals.size());
    for (const auto& [id, count] : currencyTotals) sortedIds.push_back(id);
    std::sort(sortedIds.begin(), sortedIds.end());

    // Collected as draw closures first (Coin expands into up to 3
    // denomination tiles) so the wrapping pass below can treat every tile
    // uniformly regardless of what currency it came from.
    std::vector<std::function<void()>> tiles;
    for (int64_t id : sortedIds) {
        const int64_t count = currencyTotals.at(id);
        if (count == 0 || !includeId(id)) continue;

        Gw2CurrencyInfo info = g_gw2Api.GetCurrencyInfo(id);
        if (applyDisplayFilters && !PassesCurrencyFilters(info, count)) continue;
        if (!MatchesSearchQuery(info.name, id, "Currency")) continue;

        if (id == SessionTracker::COIN_CURRENCY_ID) {
            AppendCoinTileDrawers(tiles, count);
            continue;
        }

        const bool dimmed = count < 0;
        const std::string label = (info.loaded && !info.name.empty()) ? info.name : ("Currency #" + std::to_string(id));
        tiles.push_back([id, count, dimmed, label, iconUrl = info.iconUrl]() {
            RenderCurrencyGridTile(id, count, dimmed, label, iconUrl);
        });
    }

    if (tiles.empty()) {
        ImGui::TextDisabled("(none)");
        ImGui::PopID();
        return;
    }

    const float tileSize = GridTileSize();
    const float windowVisibleX2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    for (size_t i = 0; i < tiles.size(); ++i) {
        ImGui::PushID((int)i);
        tiles[i]();
        ImGui::PopID();

        if (i + 1 < tiles.size()) {
            const float nextTileX2 = ImGui::GetItemRectMax().x + kGridTileSpacing + tileSize;
            if (nextTileX2 < windowVisibleX2) {
                ImGui::SameLine(0.0f, kGridTileSpacing);
            }
        }
    }
    ImGui::PopID();
}

// Total session profit: coin dropped plus the estimated sell value of every
// non-ignored item, treated as one combined "profit" figure rather than
// showing coin alone. Display filters (Filtering section) don't affect
// this, only the Ignored set does.
// Derived per-item view state, rebuilt by RefreshItemView. Building any of
// this is O(session item count) - each item needs a GetItemInfo() lookup
// (cache-mutex lock plus a Gw2ItemInfo copy, which carries three
// std::strings), and the Items list additionally does a full sort - so it's
// cached here and only recomputed periodically rather than every frame (see
// s_itemViewDirty and AddonRenderImpl).
struct ItemViewCache {
    int64_t totalProfit = 0;
    std::vector<ItemRow> favoriteRows;
    std::vector<ItemRow> itemRows;
};
static ItemViewCache g_itemView;

// Profit, Favorites, and the main Items list all draw from the same
// "every non-ignored item" set - Profit needs the raw total, Favorites
// needs just the favorited ones, Items needs the rest after the Filtering
// section's settings apply. Used to compute each with its own independent
// GetItemInfo() pass over nearly the same items, which tripled the real
// cost (a cache-mutex lock plus a 3-string copy, per item, per pass) for no
// reason - now it's one BuildItemRows() pass, split three ways in memory.
static void RefreshItemView(const std::unordered_map<int64_t, int64_t>& itemTotals, int64_t coinTotal)
{
    std::vector<ItemRow> allRows = BuildItemRows(itemTotals,
        [](int64_t id) { return !g_settings.ignoredItemIds.count(id); },
        /*applyDisplayFilters*/ false);

    int64_t total = coinTotal;
    for (const ItemRow& row : allRows) {
        total += row.estimatedValue;
    }
    g_itemView.totalProfit = total;

    g_itemView.favoriteRows.clear();
    g_itemView.itemRows.clear();
    for (ItemRow& row : allRows) {
        const bool isFavorite = g_settings.favoriteItemIds.count(row.id) != 0;
        if (isFavorite) {
            g_itemView.favoriteRows.push_back(std::move(row));
        } else if (PassesItemFilters(row.info, row.sellClass, row.count)) {
            g_itemView.itemRows.push_back(std::move(row));
        }
    }
    // Favorites keep a fixed alphabetical order rather than the user's
    // chosen Sorting setting - it's a pinned list, not something you'd want
    // to reorder every time you change how the main Items list sorts.
    std::sort(g_itemView.favoriteRows.begin(), g_itemView.favoriteRows.end(),
        [](const ItemRow& a, const ItemRow& b) { return a.info.name < b.info.name; });
    SortItemRows(g_itemView.itemRows);
}

// Toggle button for the search box, drawn next to Reset Session. No icon
// font is loaded in this addon (see CLAUDE.md's vendor/imgui note - only
// Nexus's own bundled ImGui build is shared across the DLL boundary, with
// whatever glyph ranges its default font atlas happens to cover), so rather
// than risk a magnifying-glass glyph rendering as tofu, this draws one
// directly with ImDrawList - same approach as DrawGridBadge/
// RenderCoinDenominationTile elsewhere in this file. `active` just tints the
// button to show the search box is currently open.
static bool RenderSearchToggleButton(bool active)
{
    const float size = ImGui::GetFrameHeight();
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    const bool clicked = ImGui::Button("##SearchToggle", ImVec2(size, size));
    if (active) {
        ImGui::PopStyleColor();
    }

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float radius = size * 0.22f;
    // The handle sticks out past the circle on the bottom-right (from
    // 0.75r to 2.35r along the diagonal) with nothing balancing it on the
    // top-left (where the circle only reaches r) - so the glyph's visual
    // centroid sits off-center from the circle's own center. Shift the
    // circle up-left by half that imbalance so the whole glyph centers
    // evenly in the button instead of crowding the bottom-right corner.
    constexpr float kHandleStartFrac = 0.75f;
    constexpr float kHandleLengthFrac = 1.6f;
    const float handleReach = (kHandleStartFrac + kHandleLengthFrac) * radius;
    const float centroidOffset = (handleReach - radius) * 0.5f;
    const ImVec2 center((min.x + max.x) * 0.5f - centroidOffset, (min.y + max.y) * 0.5f - centroidOffset);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
    drawList->AddCircle(center, radius, color, 0, 1.6f);
    const ImVec2 handleStart(center.x + radius * kHandleStartFrac, center.y + radius * kHandleStartFrac);
    const ImVec2 handleEnd(handleStart.x + radius * kHandleLengthFrac, handleStart.y + radius * kHandleLengthFrac);
    drawList->AddLine(handleStart, handleEnd, color, 1.8f);

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Search favorites, currencies, items");
    }
    return clicked;
}

static void AddonRenderImpl();
static void AddonOptionsImpl();

// Nexus calls these every frame on its own render thread, independent of
// whatever thread Load()/Unload() run on - neither previously took
// g_lifecycleMutex, so a Load()/Unload() in progress (e.g. Load()
// reassigning g_settings wholesale, or Unload() mid-Stop()) could run
// concurrently with a render frame reading/mutating that same g_settings
// (a plain struct holding std::unordered_set/std::string, not thread-safe)
// or the g_gw2Api/g_iconCache/g_drfClient globals. That's a real crash, not
// just a stale-frame glitch: reassigning a std::unordered_set on one thread
// while another iterates it corrupts the heap. try_lock (not a blocking
// lock) so a Load()/Unload() in flight just skips this frame's render
// instead of stalling the render thread for however long that takes.
static void AddonRender()
{
    std::unique_lock<std::recursive_mutex> lifecycleLock(g_lifecycleMutex, std::try_to_lock);
    if (!lifecycleLock.owns_lock() || !g_loaded) {
        return;
    }
    try {
        AddonRenderImpl();
    } catch (const std::exception& e) {
        g_api->Log(LOGL_CRITICAL, "DrfGoldTracker", (std::string("AddonRender() failed: ") + e.what()).c_str());
    } catch (...) {
        g_api->Log(LOGL_CRITICAL, "DrfGoldTracker", "AddonRender() failed: unknown exception");
    }
}

static void AddonOptions()
{
    std::unique_lock<std::recursive_mutex> lifecycleLock(g_lifecycleMutex, std::try_to_lock);
    if (!lifecycleLock.owns_lock() || !g_loaded) {
        return;
    }
    try {
        AddonOptionsImpl();
    } catch (const std::exception& e) {
        g_api->Log(LOGL_CRITICAL, "DrfGoldTracker", (std::string("AddonOptions() failed: ") + e.what()).c_str());
    } catch (...) {
        g_api->Log(LOGL_CRITICAL, "DrfGoldTracker", "AddonOptions() failed: unknown exception");
    }
}

static void AddonRenderImpl()
{
    // Make sure the QuickAccess icon textures exist as soon as possible,
    // even while the main window is hidden - GetOrCreateFromMemory is the
    // documented "get or create" pattern, safe to call every frame until it
    // stops returning null.
    g_api->Textures_GetOrCreateFromMemory(kQuickAccessIconId,
        (void*)kQuickAccessIconNormal, kQuickAccessIconNormalSize);
    g_api->Textures_GetOrCreateFromMemory(kQuickAccessIconHoverId,
        (void*)kQuickAccessIconHover, kQuickAccessIconHoverSize);
    g_goldCoinTexture = g_api->Textures_GetOrCreateFromMemory(kGoldCoinIconId,
        (void*)kGoldCoinIcon, kGoldCoinIconSize);
    g_silverCoinTexture = g_api->Textures_GetOrCreateFromMemory(kSilverCoinIconId,
        (void*)kSilverCoinIcon, kSilverCoinIconSize);
    g_copperCoinTexture = g_api->Textures_GetOrCreateFromMemory(kCopperCoinIconId,
        (void*)kCopperCoinIcon, kCopperCoinIconSize);

    // Pull anything DRF has pushed since last frame into the running totals.
    // Deliberately NOT marking s_itemViewDirty here: while actively farming,
    // drops can arrive on nearly every frame, and forcing an immediate
    // rebuild on each one would defeat RefreshItemView's throttle exactly
    // when it matters most - a new drop just rides along on the next
    // periodic refresh (at most kItemViewRefreshInterval later), which is
    // imperceptible for a stat readout.
    for (DrfDrop& drop : g_drfClient.DrainDrops()) {
        g_sessionTracker.ApplyDrop(drop.items, drop.currencies);
    }

    // The actual autosave runs off-thread (see AutosaveWorkerLoop) so it
    // can't stall rendering - this just covers the automatic-reset boundary
    // check, which runs regardless of window visibility so a daily/weekly
    // reset still fires while the window is hidden.
    static auto s_lastResetCheck = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - s_lastResetCheck >= std::chrono::seconds(30)) {
        CheckAndApplyAutomaticReset(/*isAddonLoad*/ false);
        s_lastResetCheck = now;
    }

    if (!g_settings.windowVisible) {
        return;
    }

    // RefreshItemView() (and EnsureItemsRequested's own walk of every item
    // id) costs O(session item count) - on a long-running session with
    // hundreds of items that's enough to blow the frame budget if it runs
    // every single frame, which is what made scrolling feel sluggish even
    // after the row rendering itself was clipped to visible rows. A farming
    // tracker's numbers don't need literal 60Hz freshness, so this only
    // actually recomputes on a short timer or when s_itemViewDirty says
    // something changed (see its declaration for what sets it).
    static auto s_lastItemViewRefresh = std::chrono::steady_clock::now() - std::chrono::hours(1);
    constexpr auto kItemViewRefreshInterval = std::chrono::milliseconds(250);
    if (s_itemViewDirty || now - s_lastItemViewRefresh >= kItemViewRefreshInterval) {
        auto itemTotals = g_sessionTracker.GetItemTotals();

        std::vector<int64_t> itemIds;
        itemIds.reserve(itemTotals.size());
        for (const auto& [id, count] : itemTotals) {
            itemIds.push_back(id);
        }
        g_gw2Api.EnsureItemsRequested(itemIds);
        RefreshItemView(itemTotals, g_sessionTracker.GetCoinTotal());

        s_lastItemViewRefresh = now;
        s_itemViewDirty = false;
    }

    g_texturesCreatedThisFrame = 0;
    bool windowOpen = true;
    if (ImGui::Begin("DRF Gold Tracker", &windowOpen)) {
        // Nexus's shared ImGui theme can leave PopupBorderSize at 0, so the
        // Favorite/Ignore right-click menus would otherwise blend into
        // whatever's behind them - force a thin border for every popup this
        // window opens, same reasoning as RenderItemTooltip's WindowBorderSize.
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        const DrfConnectionStatus status = g_drfClient.GetStatus();
        ImGui::TextColored(StatusColor(status), "%s", StatusLabel(status));

        // Below this many consecutive network-level failures in a row, a
        // single dropped request isn't worth alarming the user about - only
        // flag it once it looks like a genuine outage/connectivity problem.
        constexpr int GW2_API_TROUBLE_THRESHOLD = 3;
        if (g_gw2Api.GetConsecutiveFailures() >= GW2_API_TROUBLE_THRESHOLD) {
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f),
                                "GW2 API unreachable - item/currency names and prices may be stale");
        }

        const int64_t elapsedSeconds = g_sessionTracker.GetElapsedSeconds();
        ImGui::Text("Session: %02lld:%02lld:%02lld",
                    (long long)(elapsedSeconds / 3600),
                    (long long)((elapsedSeconds / 60) % 60),
                    (long long)(elapsedSeconds % 60));

        const int64_t totalProfit = g_itemView.totalProfit;
        ImGui::Text("Profit:");
        ImGui::SameLine();
        RenderCoin(totalProfit);

        const double elapsedHours = elapsedSeconds > 0 ? elapsedSeconds / 3600.0 : 0.0;
        ImGui::Text("Profit / hour:");
        ImGui::SameLine();
        RenderCoin(elapsedHours > 0.0 ? static_cast<int64_t>(totalProfit / elapsedHours) : 0);

        if (ImGui::Button("Reset Session")) {
            g_sessionTracker.Reset();
            SaveSessionState();
            s_itemViewDirty = true;
        }
        ImGui::SameLine();
        if (RenderSearchToggleButton(s_searchBoxOpen)) {
            s_searchBoxOpen = !s_searchBoxOpen;
            if (s_searchBoxOpen) {
                s_focusSearchBox = true;
            } else {
                s_searchBuf[0] = '\0';
            }
        }
        if (s_searchBoxOpen) {
            if (s_focusSearchBox) {
                ImGui::SetKeyboardFocusHere();
                s_focusSearchBox = false;
            }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##Search", "Search favorites, currencies, items...", s_searchBuf, sizeof(s_searchBuf));
        }

        ImGui::Separator();
        if (ImGui::CollapsingHeader("Favorites", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (g_settings.favoriteItemIds.empty() && g_settings.favoriteCurrencyIds.empty()) {
                ImGui::TextDisabled("Right-click an item or currency to add it here");
            } else if (g_settings.iconOnlyView) {
                if (!g_settings.favoriteCurrencyIds.empty()) {
                    RenderCurrencyIconGrid("FavoriteCurrencyGrid", g_sessionTracker.GetCurrencyTotals(),
                        [](int64_t id) { return g_settings.favoriteCurrencyIds.count(id) != 0; },
                        /*applyDisplayFilters*/ false);
                }
                if (!g_settings.favoriteItemIds.empty()) {
                    RenderItemIconGrid("FavoriteItemGrid", g_itemView.favoriteRows);
                }
            } else {
                if (!g_settings.favoriteCurrencyIds.empty()) {
                    RenderCurrencyRows("FavoriteCurrencyTable", g_sessionTracker.GetCurrencyTotals(),
                        [](int64_t id) { return g_settings.favoriteCurrencyIds.count(id) != 0; },
                        /*applyDisplayFilters*/ false);
                }
                if (!g_settings.favoriteItemIds.empty()) {
                    RenderItemRows("FavoriteItemTable", g_itemView.favoriteRows, /*enableSort*/ false);
                }
            }
        }

        ImGui::Separator();
        if (ImGui::CollapsingHeader("Currencies", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto includeCurrency = [](int64_t id) { return !g_settings.ignoredCurrencyIds.count(id) && !g_settings.favoriteCurrencyIds.count(id); };
            if (g_settings.iconOnlyView) {
                RenderCurrencyIconGrid("CurrencyGrid", g_sessionTracker.GetCurrencyTotals(), includeCurrency, /*applyDisplayFilters*/ true);
            } else {
                RenderCurrencyRows("CurrencyTable", g_sessionTracker.GetCurrencyTotals(), includeCurrency, /*applyDisplayFilters*/ true);
            }
        }

        ImGui::Separator();
        if (ImGui::CollapsingHeader("Items", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (g_settings.iconOnlyView) {
                RenderItemIconGrid("ItemGrid", g_itemView.itemRows);
            } else {
                RenderItemRows("ItemTable", g_itemView.itemRows, /*enableSort*/ true);
            }
        }

        if (!g_settings.ignoredItemIds.empty() || !g_settings.ignoredCurrencyIds.empty()) {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Ignored")) {
                for (int64_t id : std::vector<int64_t>(g_settings.ignoredItemIds.begin(), g_settings.ignoredItemIds.end())) {
                    Gw2ItemInfo info = g_gw2Api.GetItemInfo(id);
                    RenderItemIcon(info, /*dimmed*/ false);
                    ImGui::TextUnformatted((info.loaded && !info.name.empty()) ? info.name.c_str() : ("Item #" + std::to_string(id)).c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton(("Restore##item" + std::to_string(id)).c_str())) {
                        g_settings.ignoredItemIds.erase(id);
                        SaveSettings();
                    }
                }
                for (int64_t id : std::vector<int64_t>(g_settings.ignoredCurrencyIds.begin(), g_settings.ignoredCurrencyIds.end())) {
                    Gw2CurrencyInfo info = g_gw2Api.GetCurrencyInfo(id);
                    RenderIcon(info.iconUrl);
                    ImGui::TextUnformatted((info.loaded && !info.name.empty()) ? info.name.c_str() : ("Currency #" + std::to_string(id)).c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton(("Restore##curr" + std::to_string(id)).c_str())) {
                        g_settings.ignoredCurrencyIds.erase(id);
                        SaveSettings();
                    }
                }
            }
        }
        ImGui::PopStyleVar();
    }
    ImGui::End();

    // ImGui::Begin still returns true the same frame the [X] is clicked -
    // check the close flag unconditionally after End(), not inside Begin's
    // success branch.
    if (!windowOpen) {
        g_settings.windowVisible = false;
        SaveSettings();
    }
}

static void AddonOptionsImpl()
{
    if (ImGui::Checkbox("Show Window", &g_settings.windowVisible)) {
        SaveSettings();
    }
    if (ImGui::Checkbox("Icon only view", &g_settings.iconOnlyView)) {
        SaveSettings();
    }
    if (ImGui::Checkbox("Est. Value uses instant-sell price", &g_settings.estValueUseInstantSellPrice)) {
        SaveSettings();
    }
    if (ImGui::Checkbox("Show QuickAccess icon", &g_settings.showQuickAccessIcon)) {
        if (g_settings.showQuickAccessIcon) {
            g_api->QuickAccess_Add(kQuickAccessShortcutId, kQuickAccessIconId, kQuickAccessIconHoverId,
                kToggleWindowKeybindId, "DRF Gold Tracker");
        } else {
            g_api->QuickAccess_Remove(kQuickAccessShortcutId);
        }
        SaveSettings();
    }

    if (g_settings.iconOnlyView) {
        ImGui::Text("Icon size:");
        ImGui::SameLine();
        if (ImGui::SmallButton("-##iconGridTileScale")) {
            g_settings.iconGridTileScale = std::max(kMinIconGridTileScale, g_settings.iconGridTileScale - kIconGridTileScaleStep);
            SaveSettings();
        }
        ImGui::SameLine();
        ImGui::Text("%.1f", g_settings.iconGridTileScale);
        ImGui::SameLine();
        if (ImGui::SmallButton("+##iconGridTileScale")) {
            g_settings.iconGridTileScale = std::min(kMaxIconGridTileScale, g_settings.iconGridTileScale + kIconGridTileScaleStep);
            SaveSettings();
        }
    }

    ImGui::Separator();

    ImGui::Checkbox("Show token", &g_showToken);
    ImGuiInputTextFlags tokenFlags = g_showToken ? 0 : ImGuiInputTextFlags_Password;
    ImGui::InputText("DRF Token", g_tokenBuf, sizeof(g_tokenBuf), tokenFlags);

    ImGui::TextWrapped(
        "Get your DRF token from https://drf.rs");

    if (ImGui::Button("Save && Connect")) {
        g_settings.drfToken = g_tokenBuf;
        SaveSettings();
        g_drfClient.SetToken(g_settings.drfToken);
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Filtering")) {
        ImGui::TextDisabled("Check a box to hide that category");

        ImGui::TextUnformatted("Sell method");
        bool changed = false;
        changed |= ImGui::Checkbox("Hide vendor-only", &g_settings.filterHideVendorSellable);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Hide TP-sellable", &g_settings.filterHideTpSellable);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Hide not sellable", &g_settings.filterHideNotSellable);

        ImGui::TextUnformatted("Count sign");
        changed |= ImGui::Checkbox("Hide positive##count", &g_settings.filterHidePositiveCount);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Hide negative##count", &g_settings.filterHideNegativeCount);

        ImGui::TextUnformatted("Known by GW2 API");
        changed |= ImGui::Checkbox("Hide known", &g_settings.filterHideKnownByApi);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Hide unknown", &g_settings.filterHideUnknownByApi);

        ImGui::TextUnformatted("Rarity");
        for (const char* rarity : { "Junk", "Basic", "Fine", "Masterwork", "Rare", "Exotic", "Ascended", "Legendary" }) {
            bool hidden = g_settings.filterHiddenRarities.count(rarity) != 0;
            if (ImGui::Checkbox(rarity, &hidden)) {
                if (hidden) g_settings.filterHiddenRarities.insert(rarity);
                else g_settings.filterHiddenRarities.erase(rarity);
                changed = true;
            }
            if (strcmp(rarity, "Masterwork") != 0 && strcmp(rarity, "Legendary") != 0) {
                ImGui::SameLine();
            }
        }

        if (changed) {
            SaveSettings();
        }
    }

    if (ImGui::CollapsingHeader("Automatic Reset")) {
        if (ImGui::BeginCombo("Reset schedule", AutomaticResetLabel(g_settings.automaticReset))) {
            for (AutomaticReset mode : { AutomaticReset::Never, AutomaticReset::OnAddonLoad,
                                          AutomaticReset::MinutesAfterUnload, AutomaticReset::OnDailyReset,
                                          AutomaticReset::OnWeeklyReset }) {
                const bool selected = g_settings.automaticReset == mode;
                if (ImGui::Selectable(AutomaticResetLabel(mode), selected)) {
                    g_settings.automaticReset = mode;
                    RecomputeNextResetDateTime();
                    SaveSettings();
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (g_settings.automaticReset == AutomaticReset::MinutesAfterUnload) {
            if (ImGui::InputInt("Minutes after unload", &g_settings.minutesUntilResetAfterUnload)) {
                g_settings.minutesUntilResetAfterUnload = std::max(1, g_settings.minutesUntilResetAfterUnload);
                SaveSettings();
            }
        }
    }
}
