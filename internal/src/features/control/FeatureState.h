// Purpose: exposes the feature-state getters and setters used as the handoff
// layer between authenticated IPC commands and runtime feature application.

// Helpful notes:
// - Callers should use setters instead of touching storage directly so clamping
//   and side effects stay centralized.
// - The implementation is atomic and intentionally lightweight for frequent
//   reads from render/game hooks.
// - Some settings represent an active flag plus a value, allowing the client to
//   keep configured values without applying them until enabled.

#pragma once
#include <cstdint>

namespace FeatureState {

bool    GetAutoAimEnabled();
int     GetAutoAimMode();
void    SetAutoAimEnabled(bool enabled);
void    SetAutoAimMode(int mode);

int     GetAutoDodgeMode();
void    SetAutoDodgeMode(int mode);
float   GetAutoDodgeHorizonMs();
void    SetAutoDodgeHorizonMs(float ms);
float   GetAutoDodgeHitboxPadding();
void    SetAutoDodgeHitboxPadding(float paddingTiles);
bool    GetAutoDodgeWallAvoid();
void    SetAutoDodgeWallAvoid(bool enabled);

bool    GetAutoAbilityEnabled();
void    SetAutoAbilityEnabled(bool enabled);
float   GetAutoAbilityMpPct();
void    SetAutoAbilityMpPct(float pctZeroTo100);
int     GetAutoAbilityWizardMode();
void    SetAutoAbilityWizardMode(int mode);

float   GetWalkTargetX();
float   GetWalkTargetY();
bool    GetWalkTargetActive();
void    SetWalkTarget(float worldX, float worldY, bool active);

bool    GetCameraZoomActive();
float   GetCameraZoomValue();
void    SetCameraZoom(bool active, float zoom);
bool    GetCameraAngleActive();
int     GetCameraAngleValue();
void    SetCameraAngle(bool active, int angle);
bool    GetCameraCenteringActive();
bool    GetCameraCentered();
void    SetCameraCentering(bool active, bool centered);

bool    GetSkinOverrideEnabled();
int     GetSkinOverrideId();
void    SetSkinOverride(bool enabled, int skinId);

int32_t GetClientDefense();
void    SetClientDefense(int32_t defense);
int32_t GetClientClassType();
void    SetClientClassType(int32_t classType);
int32_t GetClientHp();
void    SetClientHp(int32_t hp);
int32_t GetClientMaxHp();
void    SetClientMaxHp(int32_t maxHp);
int32_t GetClientObjectId();
void    SetClientObjectId(int32_t objectId);

// Authoritative player world position from proxy NEWTICK (tiles).
bool    TryGetClientPos(float& outX, float& outY);
float   GetClientPosX();
float   GetClientPosY();
void    SetClientPos(float x, float y);

// Compact wire enemy list from proxy NEWTICK (id,type,x,y,hp,mhp|...).
void    SetWireEnemySnapshot(const char* snapshot);
bool    CopyWireEnemySnapshot(char* out, int outLen, ULONGLONG* outUpdatedMs);

bool    GetProjectileNoclipEnabled();
void    SetProjectileNoclipEnabled(bool enabled);

bool    GetPlayerNoclipActive();
bool    GetPlayerNoclipEnabled();
void    SetPlayerNoclipActive(bool active);
void    SetPlayerNoclipEnabled(bool enabled);
int     GetPlayerNoclipHotkeyVk();
void    SetPlayerNoclipHotkeyVk(int vk);
int     ConsumePendingPlayerNoclipEnabled();
void    SetPendingPlayerNoclipEnabled(int enabled);

bool    GetSocketHotkeyActive();
int     GetSocketHotkeyVk();
void    SetSocketHotkeyActive(bool active);
void    SetSocketHotkeyVk(int vk);

bool    GetFollowEntityActive();
void    SetFollowEntityActive(bool active);
void    SetFollowEntityName(const char* name);
void    GetFollowEntityName(char* out, int outLen);

bool    GetGodModeEnabled();
void    SetGodModeEnabled(bool enabled);

} // namespace FeatureState
