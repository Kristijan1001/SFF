// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "hooks/cloud/CloudRedirectHost.h"
#include "config/LuaLoader.h"
#include "config/Settings.h"
#include "runtime/Logger.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

    // ── cr_api.h ABI, resolved at runtime via GetProcAddress ──────────────────
    // Mirrors CloudRedirect/src/common/cr_api.h. We link dynamically (no import
    // lib) so LumaCore builds and runs whether or not cloud_redirect.dll exists.
    typedef void (*CR_NotifyFn)(int level, const char* title, const char* message);
    using CR_InitCloudSave_t      = bool (*)(const char*, CR_NotifyFn);
    using CR_SetApps_t            = void (*)(const std::uint32_t*, std::uint32_t);
    using CR_SetAccountId_t       = void (*)(std::uint32_t);
    using CR_InstallVtableHooks_t = bool (*)();
    using CR_EnableStatsSync_t    = void (*)(bool, bool);
    using CR_NotifyAppRunning_t   = void (*)(std::uint32_t, bool);
    using CR_NotifyStatsStored_t  = void (*)(std::uint32_t);
    using CR_Shutdown_t           = void (*)();

    HMODULE                 g_dll = nullptr;
    std::atomic<bool>       g_active{ false };
    std::atomic<bool>       g_stopSync{ false };
    std::thread             g_syncThread;
    std::mutex              g_syncMtx;
    std::vector<std::uint32_t> g_lastSynced;
    std::string             g_steamPath;                  // for re-reading loginusers.vdf
    std::atomic<std::uint32_t> g_lastAccountId{ 0 };

    CR_InitCloudSave_t      p_Init          = nullptr;
    CR_SetApps_t            p_SetApps       = nullptr;
    CR_SetAccountId_t       p_SetAccountId  = nullptr;
    CR_InstallVtableHooks_t p_InstallVtable = nullptr;
    CR_EnableStatsSync_t    p_EnableStats   = nullptr;
    CR_NotifyAppRunning_t   p_NotifyRunning = nullptr;
    CR_NotifyStatsStored_t  p_NotifyStats   = nullptr;
    CR_Shutdown_t           p_Shutdown      = nullptr;

    // CloudRedirect routes its user-facing notifications here instead of
    // popping a MessageBoxA. Must be thread-safe.
    void CrNotify(int level, const char* title, const char* message) {
        const char* t = title ? title : "";
        const char* m = message ? message : "";
        if (level >= 2)
            LOG_COREIN_ERROR("\"stage\" \"CloudRedirect\" \"cr_title\" \"{}\" \"cr_msg\" \"{}\"", t, m);
        else
            LOG_COREIN_INFO("\"stage\" \"CloudRedirect\" \"cr_title\" \"{}\" \"cr_msg\" \"{}\"", t, m);
    }

    template <typename T>
    bool Resolve(T& fn, const char* name) {
        fn = reinterpret_cast<T>(GetProcAddress(g_dll, name));
        return fn != nullptr;
    }

    std::vector<std::uint32_t> CurrentApps() {
        std::vector<std::uint32_t> out;
        for (auto id : LuaLoader::GetLibraryAppIds())
            out.push_back(static_cast<std::uint32_t>(id));
        return out;
    }

    // Real logged-in account id, same value OST derives from g_localSteamId
    // (low 32 bits of the SteamID64). We read the MostRecent user from
    // config/loginusers.vdf instead of a memory scan, which is what fails on
    // CR's side ("USER_OFF_ACCOUNTID: using fallback").
    std::uint32_t ReadLoggedInAccountId(const std::string& steamPath) {
        if (steamPath.empty()) return 0;
        std::ifstream f(steamPath + "\\config\\loginusers.vdf");
        if (!f) return 0;
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

        std::uint64_t firstId = 0, mostRecentId = 0;
        size_t pos = 0;
        while (true) {
            size_t q = s.find("\"7656119", pos);          // a "<steamid64>" block key
            if (q == std::string::npos) break;
            size_t e = s.find('"', q + 1);
            if (e == std::string::npos) break;
            std::uint64_t id = strtoull(s.substr(q + 1, e - q - 1).c_str(), nullptr, 10);
            pos = e + 1;
            if (firstId == 0) firstId = id;

            size_t nextId  = s.find("\"7656119", pos);      // block ends at the next user
            size_t blockEnd = (nextId == std::string::npos) ? s.size() : nextId;
            size_t mr = s.find("\"MostRecent\"", pos);
            if (mr != std::string::npos && mr < blockEnd) {
                size_t v1 = s.find('"', mr + 12);           // opening quote of the value
                size_t v2 = (v1 == std::string::npos) ? std::string::npos : s.find('"', v1 + 1);
                if (v2 != std::string::npos && s.substr(v1 + 1, v2 - v1 - 1) == "1")
                    mostRecentId = id;
            }
        }
        std::uint64_t chosen = mostRecentId ? mostRecentId : firstId;
        return static_cast<std::uint32_t>(chosen & 0xFFFFFFFFULL);
    }

    // Hand CloudRedirect the real account id (once known / on change) so it can
    // namespace cloud storage + stats instead of falling back to account=0.
    void RefreshAccountId() {
        if (!p_SetAccountId) return;
        std::uint32_t acc = ReadLoggedInAccountId(g_steamPath);
        if (acc != 0 && g_lastAccountId.exchange(acc) != acc) {
            p_SetAccountId(acc);
            LOG_COREIN_INFO("\"stage\" \"CloudRedirect\" \"act\" \"set-account\" \"accountId\" {}", acc);
        }
    }

} // namespace

namespace CloudRedirectHost {

void SyncAppSet() {
    if (!g_active.load() || !p_SetApps) return;

    auto apps = CurrentApps();
    std::lock_guard<std::mutex> lk(g_syncMtx);

    // Only re-push when the set actually changed (CR_SetApps replaces the
    // whole namespace-app set each call).
    std::unordered_set<std::uint32_t> now(apps.begin(), apps.end());
    std::unordered_set<std::uint32_t> was(g_lastSynced.begin(), g_lastSynced.end());
    if (now == was) return;

    p_SetApps(apps.empty() ? nullptr : apps.data(), static_cast<std::uint32_t>(apps.size()));
    g_lastSynced = std::move(apps);
    LOG_COREIN_INFO("\"stage\" \"CloudRedirect\" \"act\" \"sync-apps\" \"count\" {}",
                    static_cast<unsigned>(g_lastSynced.size()));
}

void Initialize(const char* steamInstallPath) {
    if (!Settings::cloudEnabled) {
        LOG_COREIN_INFO("\"stage\" \"CloudRedirect\" \"act\" \"disabled\"");
        return;
    }
    if (g_active.load()) return;

    // cloud_redirect.dll is placed in the Steam root by CloudRedirect's
    // companion app. Prefer the explicit Steam-dir path, fall back to the
    // default loader search path.
    if (steamInstallPath && *steamInstallPath) {
        char dllPath[MAX_PATH];
        sprintf_s(dllPath, MAX_PATH, "%s\\cloud_redirect.dll", steamInstallPath);
        g_dll = LoadLibraryA(dllPath);
    }
    if (!g_dll) g_dll = LoadLibraryA("cloud_redirect.dll");
    if (!g_dll) {
        LOG_COREIN_INFO("\"stage\" \"CloudRedirect\" \"act\" \"no-dll\" err={}", GetLastError());
        return;
    }

    const bool haveRequired =
        Resolve(p_Init,     "CR_InitCloudSave") &&
        Resolve(p_SetApps,  "CR_SetApps")       &&
        Resolve(p_Shutdown, "CR_Shutdown");
    // optional exports (older builds may lack some)
    Resolve(p_SetAccountId,  "CR_SetAccountId");
    Resolve(p_InstallVtable, "CR_InstallVtableHooks");
    Resolve(p_EnableStats,   "CR_EnableStatsSync");
    Resolve(p_NotifyRunning, "CR_NotifyAppRunning");
    Resolve(p_NotifyStats,   "CR_NotifyStatsStored");

    if (!haveRequired) {
        LOG_COREIN_ERROR("\"stage\" \"CloudRedirect\" \"err\" \"missing-exports\"");
        FreeLibrary(g_dll);
        g_dll = nullptr;
        return;
    }

    // cr_api.h requires the Steam dir WITH a trailing separator; LumaCore's
    // SteamInstallPath has none, so append one or CR builds broken paths
    // (e.g. "...\Steamconfig\...") and its file ops silently fail.
    char steamRoot[MAX_PATH];
    sprintf_s(steamRoot, MAX_PATH, "%s\\", steamInstallPath ? steamInstallPath : "");

    if (!p_Init(steamRoot, &CrNotify)) {
        LOG_COREIN_ERROR("\"stage\" \"CloudRedirect\" \"err\" \"init-failed\"");
        FreeLibrary(g_dll);
        g_dll = nullptr;
        return;
    }
    g_active.store(true);
    g_steamPath = (steamInstallPath && *steamInstallPath) ? steamInstallPath : "";

    // Hand CR the real account id up front (its own scan fails -> account=0).
    // The sync thread keeps trying in case the user logs in after init.
    RefreshAccountId();

    // Stats sync is opt-in; actual behaviour is still gated by the user's own
    // CloudRedirect config. (Data input via CR_NotifyAppRunning / _StatsStored
    // is a follow-up wiring; cloud SAVES work without it.)
    if (p_EnableStats)
        p_EnableStats(Settings::cloudSyncAchievements, Settings::cloudSyncPlaytime);

    SyncAppSet();

    // Required for the slot4 (BeginFileUpload/CommitFileUpload) RPCs to work —
    // CR hooks the RPC transport itself. Needs cloud_redirect >= 2.2.5.
    if (p_InstallVtable) {
        const bool vt = p_InstallVtable();
        LOG_COREIN_INFO("\"stage\" \"CloudRedirect\" \"act\" \"vtable-hooks\" \"ok\" {}", vt ? 1 : 0);
    } else {
        LOG_COREIN_WARN("\"stage\" \"CloudRedirect\" \"warn\" \"no-vtable-export\" "
                        "\"hint\" \"uploads need cloud_redirect >= 2.2.5\"");
    }

    LOG_COREIN_INFO("\"stage\" \"CloudRedirect\" \"act\" \"active\"");

    // Poll the library set so games added/removed live (SteaMidra add-game +
    // lua hot-reload) get pushed to CR without a Steam restart. Detached so
    // Shutdown never has to join under the loader lock.
    g_stopSync.store(false);
    g_syncThread = std::thread([] {
        while (!g_stopSync.load()) {
            for (int i = 0; i < 30 && !g_stopSync.load(); ++i) Sleep(100); // ~3s, stop-responsive
            if (g_stopSync.load()) break;
            RefreshAccountId();   // catch the account id once the user logs in
            SyncAppSet();
        }
    });
    g_syncThread.detach();
}

bool IsActive() { return g_active.load(); }

void SetAccountId(std::uint32_t accountId) {
    if (g_active.load() && p_SetAccountId) p_SetAccountId(accountId);
}
void NotifyAppRunning(std::uint32_t appId, bool running) {
    if (g_active.load() && p_NotifyRunning) p_NotifyRunning(appId, running);
}
void NotifyStatsStored(std::uint32_t appId) {
    if (g_active.load() && p_NotifyStats) p_NotifyStats(appId);
}

void Shutdown() {
    g_stopSync.store(true);
    if (g_active.exchange(false) && p_Shutdown) p_Shutdown();
    // Deliberately no join()/FreeLibrary(): Shutdown runs from LumaCore::Detach
    // (loader-lock context on FreeLibrary unload). The detached sync thread sees
    // g_stopSync and exits on its own; leaving the DLL mapped is harmless.
}

} // namespace CloudRedirectHost
