#pragma once

#include <cstdint>

// MinHook detours for the three game methods involved in shot angle redirection.
// Install() resolves IL2CPP method pointers and creates hooks; safe to call
// every tick until it succeeds (self-guards with installed flag).
namespace AimHooks {

bool Install();
void Uninstall();
bool IsInstalled();

// Master toggle from AutoAim::SetEnabled — hooks stay installed but pass-through when off.
void SetAimActive(bool on);

// Called by AutoAim coordinator each tick before hooks may fire.
void SetTarget(bool hasTarget, float x, float y);

// Weapon-specific angle tweaks
void SetReverseCultStaff(bool v);
void SetOffsetColossusSword(bool v);

struct HookStats {
    uint64_t csaCalls    = 0;
    uint64_t csaRedirect = 0;
    uint64_t swaCalls    = 0;
    uint64_t swaRedirect = 0;
};
HookStats GetHookStats();

// Invoke ShootWithAngle on cooldown while wantFire is true (hooks redirect angle).
// Skipped when the user is already holding LMB. ReleaseAutoFire() on disable/uninstall.
void UpdateAutoFire(bool wantFire);
void ReleaseAutoFire();
uint64_t GetAutoFireShots();

// Candidates diagnostics
constexpr int kNumCandidates = 5;
const char* GetAutoFireCandidateName(int idx);
bool IsAutoFireCandidateResolved(int idx);
void TestCallAutoFireCandidate(int idx, bool on);
int GetActiveAutoFireCandidateIdx();
void SetActiveAutoFireCandidateIdx(int idx);

} // namespace AimHooks
