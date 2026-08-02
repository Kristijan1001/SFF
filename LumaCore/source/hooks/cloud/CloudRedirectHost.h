// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.
//
// CloudRedirectHost — loads Selectively11/CloudRedirect's prebuilt
// cloud_redirect.dll inside the Steam process and drives its third-party
// client API (see CloudRedirect/src/common/cr_api.h) so LumaCore-unlocked
// (addappid) games get working Steam Cloud saves.
//
// Integration uses CloudRedirect's OWN vtable-hook path: after init we call
// CR_InstallVtableHooks(), and CR hooks CClientUnifiedServiceTransport itself
// and services the Cloud.* RPCs. That is the same design OpenSteamTool moved to
// in PR #153, and it avoids LumaCore having to route packets: the slot4 RPCs
// (BeginFileUpload / CommitFileUpload) need synchronous responses that a
// packet-injection path cannot satisfy.
//
// Every entry point is a safe no-op unless [cloud].enabled is set AND the DLL
// loaded and CR_InitCloudSave succeeded.

#ifndef CLOUDREDIRECTHOST_H
#define CLOUDREDIRECTHOST_H

#include <cstdint>

namespace CloudRedirectHost {

    // Load + initialise. Called once from the bootstrap worker thread, after
    // the lua config is parsed and LumaCore::Attach() has installed the hooks.
    // steamInstallPath is the Steam root directory (no trailing separator).
    void Initialize(const char* steamInstallPath);

    // Re-push the current LuaLoader::GetLibraryAppIds() set to CloudRedirect.
    // Called on init and periodically so the redirected set tracks addappid()
    // changes made while Steam is running.
    void SyncAppSet();

    // True once the DLL is loaded and CR_InitCloudSave succeeded.
    bool IsActive();

    // Optional stats-sync bridges (no-ops until wired into the relevant hooks).
    void SetAccountId(std::uint32_t accountId);
    void NotifyAppRunning(std::uint32_t appId, bool running);
    void NotifyStatsStored(std::uint32_t appId);

    // Teardown. Called from LumaCore::Detach().
    void Shutdown();

}

#endif // CLOUDREDIRECTHOST_H
