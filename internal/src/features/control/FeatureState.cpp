// Purpose: owns the shared, thread-safe feature state written by IPC/control
// code and read by runtime feature applicators.

// Helpful notes:
// - Values are stored in atomics because IPC and game/runtime paths can touch
//   them from different threads.
// - Setters clamp externally supplied values before they reach gameplay systems.
// - Skin overrides are applied immediately because SkinChanger owns that live
//   side effect; most other settings are applied later by FeatureRuntime.
// - Pending player-noclip state is an edge-triggered handoff back to UI/client
//   code after the runtime hotkey toggles the value.

#include "pch-il2cpp.h"
#include "FeatureState.h"
#include "gui/tabs/TestTAB.h"
#include "SkinChanger.h"
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace {
    static int   ClampInt  (int   v, int   lo, int   hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static float ClampFloat(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
}

static std::atomic<int> s_featAutoAimEnabled{0}, s_featAutoAimMode{0}, s_featProjectileNoclipEnabled{0};
static std::atomic<int> s_featDodgeMode{0}, s_featDodgeWallAvoid{1}, s_featAutoAbilityEnabled{0}, s_featAutoAbilityWizardMode{0};
static std::atomic<int> s_featPlayerNoclipActive{0}, s_featPlayerNoclipEnabled{0}, s_featPlayerNoclipHotkeyVk{'N'}, s_pendingPlayerNoclipEnabled{-1};
static std::atomic<int> s_featSocketHotkeyActive{0}, s_featSocketHotkeyVk{'L'}, s_featWalkTargetActive{0};
static std::atomic<int> s_featCameraZoomActive{0}, s_featCameraAngleActive{0}, s_featCameraAngleValue{0}, s_featCameraCenteringActive{0}, s_featCameraCentered{1};
static std::atomic<int> s_featSkinOverrideEnabled{0}, s_featSkinOverrideId{0};
static std::atomic<float> s_featDodgeHorizonMs{800.f}, s_featDodgeHitboxPadding{0.f}, s_featAutoAbilityMpPct{0.f};
static std::atomic<float> s_featWalkTargetX{0.f}, s_featWalkTargetY{0.f}, s_featCameraZoomValue{8.f};
static std::atomic<int32_t> s_featClientDefense{static_cast<int32_t>(0x80000000u)}, s_featClientClassType{0};
static std::atomic<int32_t> s_featClientHp{0}, s_featClientMaxHp{0}, s_featClientObjectId{0};
static std::atomic<float>   s_featClientPosX{0.f}, s_featClientPosY{0.f};
static std::atomic<int>     s_featClientPosValid{0};
static std::atomic<int> s_featFollowEntityActive{0};
static std::atomic<int> s_featGodModeEnabled{0};
static char s_featFollowEntityName[32] = {};
static std::mutex s_featFollowEntityNameMu;

static char s_wireEnemySnap[4096] = {};
static std::mutex s_wireEnemySnapMu;
static std::atomic<ULONGLONG> s_wireEnemySnapMs{0};

namespace FeatureState {

bool    GetAutoAimEnabled()           { return s_featAutoAimEnabled.load(std::memory_order_relaxed) != 0; }
int     GetAutoAimMode()              { return s_featAutoAimMode.load(std::memory_order_relaxed); }
void    SetAutoAimEnabled(bool v)     { s_featAutoAimEnabled.store(v ? 1 : 0, std::memory_order_relaxed); }
void    SetAutoAimMode(int mode)      { s_featAutoAimMode.store(ClampInt(mode, 0, 3), std::memory_order_relaxed); }

int     GetAutoDodgeMode()            { return s_featDodgeMode.load(std::memory_order_relaxed); }
void    SetAutoDodgeMode(int mode)    { s_featDodgeMode.store(ClampInt(mode, 0, static_cast<int>(TestTAB::DodgeMode::RePP)), std::memory_order_relaxed); }
float   GetAutoDodgeHorizonMs()       { return s_featDodgeHorizonMs.load(std::memory_order_relaxed); }
void    SetAutoDodgeHorizonMs(float ms)             { s_featDodgeHorizonMs.store(ClampFloat(ms, 100.f, 4000.f), std::memory_order_relaxed); }
float   GetAutoDodgeHitboxPadding()                 { return s_featDodgeHitboxPadding.load(std::memory_order_relaxed); }
void    SetAutoDodgeHitboxPadding(float paddingTiles) { s_featDodgeHitboxPadding.store(ClampFloat(paddingTiles, 0.f, 1.5f), std::memory_order_relaxed); }
bool    GetAutoDodgeWallAvoid()                     { return s_featDodgeWallAvoid.load(std::memory_order_relaxed) != 0; }
void    SetAutoDodgeWallAvoid(bool v)               { s_featDodgeWallAvoid.store(v ? 1 : 0, std::memory_order_relaxed); }

bool    GetAutoAbilityEnabled()                     { return s_featAutoAbilityEnabled.load(std::memory_order_relaxed) != 0; }
void    SetAutoAbilityEnabled(bool v)               { s_featAutoAbilityEnabled.store(v ? 1 : 0, std::memory_order_relaxed); }
float   GetAutoAbilityMpPct()                       { return s_featAutoAbilityMpPct.load(std::memory_order_relaxed); }
void    SetAutoAbilityMpPct(float pct)              { s_featAutoAbilityMpPct.store(ClampFloat(pct, 0.f, 100.f), std::memory_order_relaxed); }
int     GetAutoAbilityWizardMode()                  { return s_featAutoAbilityWizardMode.load(std::memory_order_relaxed); }
void    SetAutoAbilityWizardMode(int mode)          { s_featAutoAbilityWizardMode.store(mode == 1 ? 1 : 0, std::memory_order_relaxed); }

float   GetWalkTargetX()                            { return s_featWalkTargetX.load(std::memory_order_relaxed); }
float   GetWalkTargetY()                            { return s_featWalkTargetY.load(std::memory_order_relaxed); }
bool    GetWalkTargetActive()                       { return s_featWalkTargetActive.load(std::memory_order_relaxed) != 0; }
void    SetWalkTarget(float worldX, float worldY, bool active)
{
    s_featWalkTargetX.store(worldX, std::memory_order_relaxed);
    s_featWalkTargetY.store(worldY, std::memory_order_relaxed);
    s_featWalkTargetActive.store(active ? 1 : 0, std::memory_order_relaxed);
}

bool    GetCameraZoomActive()                       { return s_featCameraZoomActive.load(std::memory_order_relaxed) != 0; }
float   GetCameraZoomValue()                        { return s_featCameraZoomValue.load(std::memory_order_relaxed); }
void    SetCameraZoom(bool active, float zoom)      { s_featCameraZoomActive.store(active ? 1 : 0, std::memory_order_relaxed); s_featCameraZoomValue.store(zoom, std::memory_order_relaxed); }
bool    GetCameraAngleActive()                      { return s_featCameraAngleActive.load(std::memory_order_relaxed) != 0; }
int     GetCameraAngleValue()                       { return s_featCameraAngleValue.load(std::memory_order_relaxed); }
void    SetCameraAngle(bool active, int angle)      { s_featCameraAngleActive.store(active ? 1 : 0, std::memory_order_relaxed); s_featCameraAngleValue.store(angle, std::memory_order_relaxed); }
bool    GetCameraCenteringActive()                  { return s_featCameraCenteringActive.load(std::memory_order_relaxed) != 0; }
bool    GetCameraCentered()                         { return s_featCameraCentered.load(std::memory_order_relaxed) != 0; }
void    SetCameraCentering(bool active, bool centered) { s_featCameraCenteringActive.store(active ? 1 : 0, std::memory_order_relaxed); s_featCameraCentered.store(centered ? 1 : 0, std::memory_order_relaxed); }

bool    GetSkinOverrideEnabled()                    { return s_featSkinOverrideEnabled.load(std::memory_order_relaxed) != 0; }
int     GetSkinOverrideId()                         { return s_featSkinOverrideId.load(std::memory_order_relaxed); }
void    SetSkinOverride(bool enabled, int skinId)   { s_featSkinOverrideEnabled.store(enabled ? 1 : 0, std::memory_order_relaxed); s_featSkinOverrideId.store(skinId, std::memory_order_relaxed); SkinChanger::SetOverride(enabled, skinId); }

int32_t GetClientDefense()                          { return s_featClientDefense.load(std::memory_order_relaxed); }
void    SetClientDefense(int32_t defense)           { s_featClientDefense.store(defense, std::memory_order_relaxed); }
int32_t GetClientClassType()                        { return s_featClientClassType.load(std::memory_order_relaxed); }
void    SetClientClassType(int32_t classType)       { s_featClientClassType.store(classType, std::memory_order_relaxed); }
int32_t GetClientHp()                               { return s_featClientHp.load(std::memory_order_relaxed); }
void    SetClientHp(int32_t hp)                     { s_featClientHp.store(hp < 0 ? 0 : hp, std::memory_order_relaxed); }
int32_t GetClientMaxHp()                            { return s_featClientMaxHp.load(std::memory_order_relaxed); }
void    SetClientMaxHp(int32_t maxHp)               { s_featClientMaxHp.store(maxHp < 0 ? 0 : maxHp, std::memory_order_relaxed); }
int32_t GetClientObjectId()                         { return s_featClientObjectId.load(std::memory_order_relaxed); }
void    SetClientObjectId(int32_t objectId)         { s_featClientObjectId.store(objectId < 0 ? 0 : objectId, std::memory_order_relaxed); }

bool    TryGetClientPos(float& outX, float& outY)
{
    if (s_featClientPosValid.load(std::memory_order_relaxed) == 0) return false;
    outX = s_featClientPosX.load(std::memory_order_relaxed);
    outY = s_featClientPosY.load(std::memory_order_relaxed);
    return std::isfinite(outX) && std::isfinite(outY);
}

float   GetClientPosX() { return s_featClientPosX.load(std::memory_order_relaxed); }
float   GetClientPosY() { return s_featClientPosY.load(std::memory_order_relaxed); }

void    SetClientPos(float x, float y)
{
    if (!std::isfinite(x) || !std::isfinite(y)) {
        s_featClientPosValid.store(0, std::memory_order_relaxed);
        return;
    }
    s_featClientPosX.store(x, std::memory_order_relaxed);
    s_featClientPosY.store(y, std::memory_order_relaxed);
    s_featClientPosValid.store(1, std::memory_order_relaxed);
}

bool    GetProjectileNoclipEnabled()                { return s_featProjectileNoclipEnabled.load(std::memory_order_relaxed) != 0; }
void    SetProjectileNoclipEnabled(bool v)          { s_featProjectileNoclipEnabled.store(v ? 1 : 0, std::memory_order_relaxed); }

bool    GetPlayerNoclipActive()                     { return s_featPlayerNoclipActive.load(std::memory_order_relaxed) != 0; }
bool    GetPlayerNoclipEnabled()                    { return s_featPlayerNoclipEnabled.load(std::memory_order_relaxed) != 0; }
void    SetPlayerNoclipActive(bool v)               { s_featPlayerNoclipActive.store(v ? 1 : 0, std::memory_order_relaxed); }
void    SetPlayerNoclipEnabled(bool v)              { s_featPlayerNoclipEnabled.store(v ? 1 : 0, std::memory_order_relaxed); }
int     GetPlayerNoclipHotkeyVk()                   { return s_featPlayerNoclipHotkeyVk.load(std::memory_order_relaxed); }
void    SetPlayerNoclipHotkeyVk(int vk)             { s_featPlayerNoclipHotkeyVk.store(vk, std::memory_order_relaxed); }
int     ConsumePendingPlayerNoclipEnabled()         { return s_pendingPlayerNoclipEnabled.exchange(-1, std::memory_order_relaxed); }
void    SetPendingPlayerNoclipEnabled(int enabled)  { s_pendingPlayerNoclipEnabled.store(enabled, std::memory_order_relaxed); }

bool    GetSocketHotkeyActive()                     { return s_featSocketHotkeyActive.load(std::memory_order_relaxed) != 0; }
int     GetSocketHotkeyVk()                         { return s_featSocketHotkeyVk.load(std::memory_order_relaxed); }
void    SetSocketHotkeyActive(bool v)               { s_featSocketHotkeyActive.store(v ? 1 : 0, std::memory_order_relaxed); }
void    SetSocketHotkeyVk(int vk)                   { s_featSocketHotkeyVk.store(vk, std::memory_order_relaxed); }

bool    GetFollowEntityActive()                     { return s_featFollowEntityActive.load(std::memory_order_relaxed) != 0; }
void    SetFollowEntityActive(bool v)               { s_featFollowEntityActive.store(v ? 1 : 0, std::memory_order_relaxed); }
void    SetFollowEntityName(const char* name)
{
    std::lock_guard<std::mutex> lk(s_featFollowEntityNameMu);
    if (!name || !name[0]) { s_featFollowEntityName[0] = '\0'; return; }
    strncpy_s(s_featFollowEntityName, name, _TRUNCATE);
}
void    GetFollowEntityName(char* out, int outLen)
{
    if (!out || outLen <= 0) return;
    std::lock_guard<std::mutex> lk(s_featFollowEntityNameMu);
    strncpy_s(out, outLen, s_featFollowEntityName, _TRUNCATE);
}

bool    GetGodModeEnabled()                           { return s_featGodModeEnabled.load(std::memory_order_relaxed) != 0; }
void    SetGodModeEnabled(bool v)                   { s_featGodModeEnabled.store(v ? 1 : 0, std::memory_order_relaxed); }

void SetWireEnemySnapshot(const char* snapshot)
{
    std::lock_guard<std::mutex> lk(s_wireEnemySnapMu);
    if (!snapshot || !snapshot[0]) {
        s_wireEnemySnap[0] = '\0';
    } else {
        strncpy_s(s_wireEnemySnap, snapshot, _TRUNCATE);
    }
    s_wireEnemySnapMs.store(GetTickCount64(), std::memory_order_relaxed);
}

bool CopyWireEnemySnapshot(char* out, int outLen, ULONGLONG* outUpdatedMs)
{
    if (!out || outLen <= 0) return false;
    std::lock_guard<std::mutex> lk(s_wireEnemySnapMu);
    strncpy_s(out, static_cast<size_t>(outLen), s_wireEnemySnap, _TRUNCATE);
    if (outUpdatedMs)
        *outUpdatedMs = s_wireEnemySnapMs.load(std::memory_order_relaxed);
    return s_wireEnemySnap[0] != '\0';
}

} // namespace FeatureState
