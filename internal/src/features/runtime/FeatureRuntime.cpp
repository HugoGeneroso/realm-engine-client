// Purpose: applies feature state changes on the game/runtime side once the IPC
// bridge has accepted commands and stored them in FeatureState.

// Helpful notes:
// - FeatureState is the thread-safe handoff point from IPC/control code.
// - This file talks to live gameplay systems, GUI tabs, hooks, and services.
// - ApplyOverrides is called repeatedly, so each helper caches the last applied
//   values to avoid reapplying unchanged settings every frame.
// - Hotkey polling is gated to the foreground game process before firing events.


#include "pch-il2cpp.h"
#include "FeatureRuntime.h"
#include "FeatureState.h"
#include "FeatureCommandRegistry.h"
#include "DbgFileLog.h"
#include "FloatingTextService.h"
#include "GameState.h"
#include "LocalPlayer.h"
#include "RuntimeOffsets.h"
#include "BootGate.h"
#include "AutoAim.h"
#include "ProjNoclip.h"
#include "Noclip.h"
#include "gui/tabs/TestTAB.h"
#include "gui/tabs/CombatTab/CombatTAB.h"
#include "gui/tabs/CameraTAB.h"
#include "DangerPlanner.h"
#include "ProjectileTracking.h"

#include <limits>
#include <climits>
#include <cctype>
#include <cstring>
#include <string>

namespace WorldTAB { bool IsPlayableRealmMap(); bool ReadMapName(char* buf, int bufLen); bool FindPlayerByName(const char* query, int32_t& outObjectId, char* outMatchedName, int nameLen); void ForceRefresh(); bool TryResolveLocalFromProxyOid(int32_t proxyOid); }

namespace {

    void ApplyAutoAimFeatureState()
    {
        static int s_lastEnabled = -1, s_lastMode = -1;
        const int enabled = FeatureState::GetAutoAimEnabled() ? 1 : 0;
        if (enabled != s_lastEnabled) {
            if (enabled != 0 && !GameState::GetLocalPtr() && !WorldTAB::IsPlayableRealmMap()) {
                static int s_deferN = 0;
                if ((s_deferN++ % 120) == 0)
                    DBG_FILE_LOG("[AutoAim] IPC defer enable (hub/menu — wait for realm)");
                return;
            }
            if (enabled != 0 && !BootGate::FeatureAllowed("ProjectileTracking")) {
                RuntimeOffsets::KickAsyncStructuralScan();
            }
            s_lastEnabled = enabled;
            DBG_FILE_LOG("[AutoAim] IPC autoAimEnabled=" << (enabled != 0));
            AutoAim::SetEnabled(enabled != 0);
        }
        const int aimMode = FeatureState::GetAutoAimMode();
        if (aimMode != s_lastMode) {
            s_lastMode = aimMode;
            AutoAim::AimMode resolved = AutoAim::AimMode::ClosestToPlayer;
            if (aimMode == 1)      resolved = AutoAim::AimMode::HighestHP;
            else if (aimMode == 2) resolved = AutoAim::AimMode::ClosestToMouse;
            else if (aimMode == 3) resolved = AutoAim::AimMode::Locked;
            AutoAim::SetAimMode(resolved);
        }
    }

    void ApplyProjectileNoclipFeatureState()
    {
        static int s_lastEnabled = -1;
        const int enabled = FeatureState::GetProjectileNoclipEnabled() ? 1 : 0;
        if (enabled != 0 && !ProjNoclip::IsInstalled()) ProjNoclip::Install();
        if (enabled != s_lastEnabled || ProjNoclip::IsEnabled() != (enabled != 0)) { s_lastEnabled = enabled; ProjNoclip::SetEnabled(enabled != 0); }
    }

    void ApplyAutoDodgeFeatureState()
    {
        // Start at Off — the dashboard's first IPC sync also sends 0; treating
        // INT32_MIN as "unset" used to run ApplyDodgeModeWithEnter on every boot
        // and hang/crash the render thread in the SetEnabled batch (~4s freeze).
        static int s_lastMode = static_cast<int>(TestTAB::DodgeMode::Off);
        static float s_lastHorizonMs = -1.f;
        int dodgeMode = FeatureState::GetAutoDodgeMode();
        if (dodgeMode != s_lastMode) {
            // Require a live character (maxHp > 0), not just a transient local ptr.
            // Enabling dodge in Nexus triggered il2cpp_class_for_each and hung ~53s.
            if (dodgeMode != static_cast<int>(TestTAB::DodgeMode::Off)) {
                char mapName[64] = {};
                WorldTAB::ReadMapName(mapName, sizeof(mapName));
                const bool inRealm = WorldTAB::IsPlayableRealmMap();
                const int32_t proxyOid = FeatureState::GetClientObjectId();
                const int wireMaxHp = FeatureState::GetClientMaxHp();
                // Never ForceRefresh on render thread during portal load — defer until
                // proxy NEWTICK confirms the new realm character (objectId + maxHp).
                if (!inRealm || proxyOid <= 0 || wireMaxHp <= 0) {
                    static int s_deferN = 0;
                    if ((s_deferN++ % 120) == 0)
                        DBG_FILE_LOG("[DodgeSwap] dodge deferred (need realm wire player oid="
                            << proxyOid << " maxHp=" << wireMaxHp
                            << " map=" << (mapName[0] ? mapName : "?") << ")");
                    return;
                }
                if (proxyOid > 0)
                    WorldTAB::TryResolveLocalFromProxyOid(proxyOid);
                if (!BootGate::FeatureAllowed("ProjectileTracking")) {
                    static int s_deferN = 0;
                    if ((s_deferN++ % 120) == 0)
                        DBG_FILE_LOG("[DodgeSwap] dodge deferred (projectile offsets not ready — "
                            "HBEAKBIHANL stale)");
                    RuntimeOffsets::KickAsyncStructuralScan();
                    return;
                }
            }
            DBG_FILE_LOG("[DodgeSwap] IPC autoDodgeMode changed " << s_lastMode << " -> " << dodgeMode
                << " (this is the raw index the dashboard sent; 4=ZDodge 5=RePP)");
            s_lastMode = dodgeMode;
            TestTAB::SetDodgeModeWithEnter(static_cast<TestTAB::DodgeMode>(dodgeMode));
            DBG_FILE_LOG("[DodgeSwap] IPC SetDodgeModeWithEnter returned");
        }
        if (dodgeMode != static_cast<int>(TestTAB::DodgeMode::Off)) {
            DangerPlanner::TickDeferredInstall();
        }
        float horizonMs = FeatureState::GetAutoDodgeHorizonMs();
        if (horizonMs != s_lastHorizonMs) { s_lastHorizonMs = horizonMs; TestTAB::SetDodgeLookaheadMs(horizonMs); }
    }

    void ApplyAutoAbilityFeatureState()
    {
        static int s_lastEnabled = -1, s_lastWizMode = INT32_MIN;
        static float s_lastMpPct = -1.f;
        const int enabled = FeatureState::GetAutoAbilityEnabled() ? 1 : 0;
        const float mpPct = FeatureState::GetAutoAbilityMpPct();
        const int wizMode = FeatureState::GetAutoAbilityWizardMode();
        if (enabled != s_lastEnabled) { s_lastEnabled = enabled; CombatTAB::SetAutoAbility(enabled != 0); }
        if (mpPct != s_lastMpPct) { s_lastMpPct = mpPct; CombatTAB::SetAbilityMpPct(mpPct); }
        if (wizMode != s_lastWizMode) { s_lastWizMode = wizMode; CombatTAB::SetWizardAbilityTargetMode(wizMode); }
    }

    bool IsCurrentProcessForeground()
    {
        HWND hwnd = GetForegroundWindow();
        if (!hwnd) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        return pid == GetCurrentProcessId();
    }

    static bool IsHotkeyDown(int active, int vk) { return active != 0 && vk != 0 && IsCurrentProcessForeground() && ((GetAsyncKeyState(vk) & 0x8000) != 0); }

    void ApplyPlayerNoclipFeatureState()
    {
        static int s_lastEnabled = -1;
        static bool s_lastHotkeyDown = false;
        const int active = FeatureState::GetPlayerNoclipActive() ? 1 : 0;
        int enabled = active && FeatureState::GetPlayerNoclipEnabled() ? 1 : 0;
        const int vk = FeatureState::GetPlayerNoclipHotkeyVk();
        const bool hotkeyDown = IsHotkeyDown(active, vk);
        if (hotkeyDown && !s_lastHotkeyDown) {
            enabled = enabled ? 0 : 1;
            FeatureState::SetPlayerNoclipEnabled(enabled != 0);
            FeatureState::SetPendingPlayerNoclipEnabled(enabled);
        }
        s_lastHotkeyDown = hotkeyDown;
        if (enabled != s_lastEnabled) { s_lastEnabled = enabled; Noclip::SetEnabled(enabled != 0); Noclip::SetMode(enabled != 0 ? 1 : 0); }
    }

    void ApplyProxyLocalRefresh()
    {
        static int32_t s_lastOid = 0;
        const int32_t oid = FeatureState::GetClientObjectId();
        if (oid > 0 && oid != s_lastOid) {
            s_lastOid = oid;
            // Full DoRefresh on the render thread hung ~3s in Nexus and crashed Unity.
            WorldTAB::TryResolveLocalFromProxyOid(oid);
        } else if (oid <= 0 && s_lastOid != 0) {
            s_lastOid = 0;
        }
    }

    void ApplyFollowEntityFeatureState()
    {
        static bool      s_lastActive = false;
        static char      s_lastName[32] = {};
        static int32_t   s_lastOid = 0;
        static ULONGLONG s_lastLookupMs = 0;

        const bool active = FeatureState::GetFollowEntityActive();
        char name[32] = {};
        FeatureState::GetFollowEntityName(name, sizeof(name));

        if (!active || !name[0]) {
            if (s_lastActive || s_lastOid != 0) {
                DangerPlanner::ClearFollowPlayer();
                s_lastOid = 0;
            }
            s_lastActive = false;
            s_lastName[0] = '\0';
            return;
        }

        if (!WorldTAB::IsPlayableRealmMap()) return;

        const ULONGLONG now = GetTickCount64();
        const bool nameChanged = strcmp(name, s_lastName) != 0;
        if (nameChanged || now - s_lastLookupMs > 500) {
            s_lastLookupMs = now;
            WorldTAB::ForceRefresh();
            int32_t oid = 0;
            char matched[32] = {};
            if (WorldTAB::FindPlayerByName(name, oid, matched, sizeof(matched))) {
                if (oid != s_lastOid) {
                    DangerPlanner::SetFollowPlayer(oid);
                    s_lastOid = oid;
                }
            }
            strncpy_s(s_lastName, name, _TRUNCATE);
        }
        s_lastActive = true;
    }

    void ApplyWalkTargetFeatureState()
    {
        static int   s_lastActive = -1;
        static float s_lastX      = std::numeric_limits<float>::quiet_NaN();
        static float s_lastY      = std::numeric_limits<float>::quiet_NaN();
        const int   walkActive = FeatureState::GetWalkTargetActive() ? 1 : 0;
        const float walkX      = FeatureState::GetWalkTargetX();
        const float walkY      = FeatureState::GetWalkTargetY();
        const bool changed = walkActive != s_lastActive || walkX != s_lastX || walkY != s_lastY;
        if (changed) {
            s_lastActive = walkActive; s_lastX = walkX; s_lastY = walkY;
            TestTAB::SetBotWalkTarget(walkX, walkY, walkActive != 0);
        }
    }

    void ApplyCameraFeatureState()
    {
        static int zoomActive = -1, angleActive = -1, centerActive = -1, angle = INT32_MIN, centered = -1;
        static float zoom = std::numeric_limits<float>::quiet_NaN();
        {
            const int active = FeatureState::GetCameraZoomActive() ? 1 : 0;
            const float value = FeatureState::GetCameraZoomValue();
            if (active != 0 && (active != zoomActive || value != zoom)) { zoomActive = active; zoom = value; CameraTAB::SetZoomValue(value); }
            else if (active == 0) zoomActive = active;
        }
        {
            const int active = FeatureState::GetCameraAngleActive() ? 1 : 0;
            const int value = FeatureState::GetCameraAngleValue();
            if (active != 0 && (active != angleActive || value != angle)) { angleActive = active; angle = value; CameraTAB::SetAngleDegrees(value); }
            else if (active == 0) angleActive = active;
        }
        {
            const int active = FeatureState::GetCameraCenteringActive() ? 1 : 0;
            const int value = FeatureState::GetCameraCentered() ? 1 : 0;
            if (active != 0 && (active != centerActive || value != centered)) { centerActive = active; centered = value; CameraTAB::SetCenteredOnPlayer(value != 0); }
            else if (active == 0) centerActive = active;
        }
    }

    // ── Plugin toggle hotkeys (owner feature) ────────────────────────────────
    struct PluginToggleHotkeyBinding {
        char pluginId[96];
        int  mainVk;
        bool requireShift;
        bool requireCtrl;
        bool requireAlt;
        bool lastDown;
    };
    std::vector<PluginToggleHotkeyBinding> s_pluginToggleHotkeys;

    bool IsVkDown(int vk)    { return ((GetAsyncKeyState(vk) | GetKeyState(vk)) & 0x8000) != 0; }
    bool IsShiftDown()       { return IsVkDown(VK_SHIFT)   || IsVkDown(VK_LSHIFT)   || IsVkDown(VK_RSHIFT); }
    bool IsCtrlDown()        { return IsVkDown(VK_CONTROL) || IsVkDown(VK_LCONTROL) || IsVkDown(VK_RCONTROL); }
    bool IsAltDown()         { return IsVkDown(VK_MENU)    || IsVkDown(VK_LMENU)    || IsVkDown(VK_RMENU); }

    bool ParsePluginToggleHotkey(const char* raw, PluginToggleHotkeyBinding& binding)
    {
        if (!raw) return false;
        std::string input(raw);
        size_t start = 0;
        int mainVk = 0;
        bool requireShift = false, requireCtrl = false, requireAlt = false;
        while (start <= input.size()) {
            const size_t end = input.find('+', start);
            std::string part = input.substr(start, end == std::string::npos ? std::string::npos : end - start);
            std::string compact;
            for (const char ch : part) {
                const unsigned char c = static_cast<unsigned char>(ch);
                if (!std::isspace(c)) compact.push_back(static_cast<char>(std::toupper(c)));
            }
            if (!compact.empty()) {
                if (compact == "SHIFT") {
                    requireShift = true;
                } else if (compact == "CTRL" || compact == "CONTROL") {
                    requireCtrl = true;
                } else if (compact == "ALT" || compact == "MENU") {
                    requireAlt = true;
                } else {
                    if (mainVk != 0) return false;
                    mainVk = FeatureCommandRegistry::ResolveHotkeyVk(compact.c_str());
                    if (mainVk == VK_SHIFT || mainVk == VK_CONTROL || mainVk == VK_MENU) return false;
                }
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (mainVk == 0) return false;
        binding.mainVk = mainVk;
        binding.requireShift = requireShift;
        binding.requireCtrl = requireCtrl;
        binding.requireAlt = requireAlt;
        return true;
    }

    bool IsPluginToggleHotkeyDown(const PluginToggleHotkeyBinding& binding, bool foreground)
    {
        if (!foreground || binding.mainVk == 0) return false;
        if (binding.requireShift && !IsShiftDown()) return false;
        if (binding.requireCtrl && !IsCtrlDown()) return false;
        if (binding.requireAlt && !IsAltDown()) return false;
        return IsVkDown(binding.mainVk);
    }

    bool IsPluginHotkeyIdSafe(const char* s)
    {
        if (!s || !*s) return false;
        const size_t len = strlen(s);
        if (len >= 96) return false;
        for (size_t i = 0; i < len; ++i) {
            const char c = s[i];
            const bool ok =
                (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
            if (!ok) return false;
        }
        return true;
    }

} // namespace

bool FeatureRuntime::PollSocketHotkeyEvent()
{
    static bool s_lastHotkeyDown = false;
    const int active = FeatureState::GetSocketHotkeyActive() ? 1 : 0;
    const int vk = FeatureState::GetSocketHotkeyVk();
    const bool hotkeyDown = IsHotkeyDown(active, vk);
    const bool shouldFire = hotkeyDown && !s_lastHotkeyDown;
    s_lastHotkeyDown = hotkeyDown;
    return shouldFire;
}

void FeatureRuntime::ApplyPluginToggleHotkeys(const char* spec)
{
    s_pluginToggleHotkeys.clear();
    if (!spec || !*spec) return;

    std::string input(spec);
    size_t start = 0;
    while (start < input.size()) {
        const size_t end = input.find(';', start);
        const std::string token = input.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const size_t eq = token.find('=');
        if (eq != std::string::npos) {
            const std::string pluginId = token.substr(0, eq);
            const std::string hotkey = token.substr(eq + 1);
            if (IsPluginHotkeyIdSafe(pluginId.c_str())) {
                PluginToggleHotkeyBinding binding{};
                if (ParsePluginToggleHotkey(hotkey.c_str(), binding)) {
                    strncpy_s(binding.pluginId, sizeof(binding.pluginId), pluginId.c_str(), _TRUNCATE);
                    binding.lastDown = IsPluginToggleHotkeyDown(binding, IsCurrentProcessForeground());
                    s_pluginToggleHotkeys.push_back(binding);
                }
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

void FeatureRuntime::CollectPluginToggleHotkeyEvents(std::vector<std::string>& outPluginIds)
{
    if (s_pluginToggleHotkeys.empty()) return;
    const bool foreground = IsCurrentProcessForeground();
    for (auto& binding : s_pluginToggleHotkeys) {
        const bool down = IsPluginToggleHotkeyDown(binding, foreground);
        if (down && !binding.lastDown) outPluginIds.emplace_back(binding.pluginId);
        binding.lastDown = down;
    }
}

void FeatureRuntime::ApplyOverrides()
{
    ApplyPlayerNoclipFeatureState();
    ApplyAutoAimFeatureState();
    ApplyProjectileNoclipFeatureState();
    ApplyProxyLocalRefresh();
    ApplyAutoDodgeFeatureState();
    if (FeatureState::GetAutoDodgeMode() != 0 || FeatureState::GetAutoAimEnabled())
        ProjectileTracking::TickDeferredInstall();
    if (GameState::GetWorldMgr() == nullptr) return;
    ApplyFollowEntityFeatureState();
    if (GameState::GetLocalPtr() == nullptr) return;
    ApplyAutoAbilityFeatureState();
    ApplyWalkTargetFeatureState(); ApplyCameraFeatureState(); FloatingTextService::ApplyPendingPluginText();
}
