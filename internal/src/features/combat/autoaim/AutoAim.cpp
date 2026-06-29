#include "pch-il2cpp.h"

#include "AutoAim.h"
#include "AimHooks.h"
#include "WeaponProfile.h"
#include "TargetSelector.h"
#include "features/combat/enemytracker/EnemyTracker.h"
#include "GameState.h"
#include "RuntimeOffsets.h"
#include "BootGate.h"
#include "ProjectileTracking.h"
#include "AoeTracking.h"
#include "DbgFileLog.h"
#include "DangerPlanner.h"
#include "FeatureState.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace {

static inline bool AddrOk(const void* p) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    return a > 0x10000 && a < 0x7FFFFFFFFFFFULL;
}

// ── Frame-level state ─────────────────────────────────────────────────────────
static std::atomic<bool>    s_enabled{ false };
static std::atomic<int>     s_aimModeInt{ 0 };
static std::atomic<int32_t> s_lockedEnemyId{ -1 };

static std::atomic<bool>    s_shootInvulnerable{ false };
static std::atomic<bool>    s_prioritizeBosses{ false };
static std::atomic<bool>    s_ignoreWalls{ true };
static std::atomic<bool>    s_shootWhileStealthed{ true };
static std::atomic<bool>    s_mouseBoundingEnabled{ true };
static std::atomic<float>   s_mouseBoundingRange{ 2.f };
static std::atomic<float>   s_rangeLeadBias{ 1.f };
static std::atomic<bool>    s_reverseCultStaff{ true };
static std::atomic<bool>    s_offsetColossus{ false };

static std::atomic<bool>    s_hasTarget{ false };
static std::atomic<float>   s_aimX{ 0.f };
static std::atomic<float>   s_aimY{ 0.f };
static std::atomic<int32_t> s_aimFocusId{ 0 };

static const int32_t*       s_skipObjTypes = nullptr;
static int                  s_skipObjCount = 0;

static ULONGLONG s_lastThrottleMs = 0;

// Plugin IPC, FeatureState mirror, and/or dodge enemy-auto-lock (xdodgeAutoLock >= 1).
static bool IsAimRequested()
{
    if (FeatureState::GetAutoAimEnabled()) return true;
    if (s_enabled.load(std::memory_order_relaxed)) return true;
    if (FeatureState::GetAutoDodgeMode() == 0) return false;
    return DangerPlanner::GetAutoLockMode() >= 1;
}

static TargetSelector::Mode EffectiveAimMode()
{
    if (s_enabled.load(std::memory_order_relaxed))
        return static_cast<TargetSelector::Mode>(s_aimModeInt.load(std::memory_order_relaxed));
    // Dodge integrated lock without the Auto Aim plugin — closest enemy for shots.
    return TargetSelector::Mode::ClosestToPlayer;
}

static bool LocalStealthBlocksAim(void* player)
{
    if (s_shootWhileStealthed.load(std::memory_order_relaxed)) return false;
    if (!AddrOk(player)) return false;
    uint32_t w0 = 0, w1 = 0;
    if (!RuntimeOffsets::TryReadMapObjectConditions(player, &w0, &w1)) return false;
    const uint64_t full = RuntimeOffsets::GetFullConditions(w0, w1);
    return RuntimeOffsets::HasCondition(full, RuntimeOffsets::ConditionEffects::Invisible);
}

// MSVC: __try/__except must live in functions without C++ locals that need unwinding.
static bool SafeWeaponCalibratorTick(void* local)
{
    __try {
        WeaponCalibrator::Tick(local);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeEnemyTrackerTick()
{
    __try {
        EnemyTracker::Tick();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadPlayerPos(void* local, float* outX, float* outY)
{
    float mx = 0.f, my = 0.f;
    bool memOk = false;
    __try {
        uint8_t* lp = reinterpret_cast<uint8_t*>(local);
        mx = *reinterpret_cast<float*>(lp + RuntimeOffsets::PosX);
        my = *reinterpret_cast<float*>(lp + RuntimeOffsets::PosY);
        memOk = std::isfinite(mx) && std::isfinite(my);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memOk = false;
    }

    float wx = 0.f, wy = 0.f;
    if (FeatureState::TryGetClientPos(wx, wy)) {
        *outX = wx;
        *outY = wy;
        return true;
    }
    if (!memOk) return false;
    *outX = mx;
    *outY = my;
    return true;
}

static bool SafeTargetSelect(const TargetSelector::Config& cfg,
                             float px, float py,
                             const WeaponProfile& weapon,
                             TargetSelector::Result* out)
{
    __try {
        *out = TargetSelector::Select(cfg, px, py, 0.f, 0.f, weapon);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void RunTick()
{
    const bool aimOn = IsAimRequested();

    void* local = GameState::GetLocalPtr();

    // Aim is inactive — clear target. Hooks must be installed and the player must
    // not be stealthed (when ShootWhileStealthed is off).
    if (!aimOn || !local || !AimHooks::IsInstalled() || LocalStealthBlocksAim(local)) {
        s_hasTarget.store(false, std::memory_order_relaxed);
        s_aimFocusId.store(0, std::memory_order_relaxed);
        AimHooks::SetTarget(false, 0.f, 0.f);
        return;
    }

    static int s_lastFailPhase = -1;
    static ULONGLONG s_lastPhaseLogMs = 0;

    if (!SafeWeaponCalibratorTick(local)) {
        const ULONGLONG now = GetTickCount64();
        if (s_lastFailPhase != 0 || now - s_lastPhaseLogMs > 2000ULL) {
            s_lastFailPhase = 0;
            s_lastPhaseLogMs = now;
            DBG_FILE_LOG("[AutoAim] RunTick SEH phase=WeaponCalibrator — clearing target");
        }
        s_hasTarget.store(false, std::memory_order_relaxed);
        s_aimFocusId.store(0, std::memory_order_relaxed);
        AimHooks::SetTarget(false, 0.f, 0.f);
        return;
    }

    float px = 0.f, py = 0.f;
    if (!SafeReadPlayerPos(local, &px, &py)) {
        const ULONGLONG now = GetTickCount64();
        if (s_lastFailPhase != 2 || now - s_lastPhaseLogMs > 2000ULL) {
            s_lastFailPhase = 2;
            s_lastPhaseLogMs = now;
            DBG_FILE_LOG("[AutoAim] RunTick SEH phase=PlayerPos — clearing target");
        }
        s_hasTarget.store(false, std::memory_order_relaxed);
        s_aimFocusId.store(0, std::memory_order_relaxed);
        AimHooks::SetTarget(false, 0.f, 0.f);
        return;
    }

    TargetSelector::Config cfg;
    cfg.mode                 = EffectiveAimMode();
    cfg.shootInvulnerable    = s_shootInvulnerable.load(std::memory_order_relaxed);
    cfg.prioritizeBosses     = s_prioritizeBosses.load(std::memory_order_relaxed);
    cfg.ignoreWalls          = s_ignoreWalls.load(std::memory_order_relaxed);
    cfg.rangeLeadBias        = s_rangeLeadBias.load(std::memory_order_relaxed);
    cfg.mouseBoundingEnabled = s_mouseBoundingEnabled.load(std::memory_order_relaxed);
    cfg.mouseBoundingRange   = s_mouseBoundingRange.load(std::memory_order_relaxed);
    cfg.lockedEnemyId        = s_lockedEnemyId.load(std::memory_order_relaxed);
    cfg.skipObjTypes         = s_skipObjTypes;
    cfg.skipObjCount         = s_skipObjCount;

    TargetSelector::Result result{};
    if (!SafeTargetSelect(cfg, px, py, WeaponCalibrator::GetProfile(), &result)) {
        const ULONGLONG now = GetTickCount64();
        if (s_lastFailPhase != 3 || now - s_lastPhaseLogMs > 2000ULL) {
            s_lastFailPhase = 3;
            s_lastPhaseLogMs = now;
            DBG_FILE_LOG("[AutoAim] RunTick SEH phase=TargetSelect — clearing target");
        }
        s_hasTarget.store(false, std::memory_order_relaxed);
        s_aimFocusId.store(0, std::memory_order_relaxed);
        AimHooks::SetTarget(false, 0.f, 0.f);
        return;
    }

    s_lastFailPhase = -1;
    s_hasTarget.store(result.found, std::memory_order_relaxed);
    s_aimX.store(result.aimX, std::memory_order_relaxed);
    s_aimY.store(result.aimY, std::memory_order_relaxed);
    s_aimFocusId.store(result.found ? result.enemyId : 0, std::memory_order_relaxed);
    AimHooks::SetTarget(result.found, result.aimX, result.aimY);
}

// __try in a separate function with no C++ locals — MSVC rejects mixing SEH with
// destructors in the same function. Header safe_call() inlined into Tick() was
// falsely tripping every frame; a local wrapper catches real AV without that.
static bool SafeInvokeRunTick()
{
    __try {
        RunTick();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool WantAutoFire(void* local)
{
    if (!local || !AimHooks::IsInstalled()) return false;
    if (!s_hasTarget.load(std::memory_order_relaxed)) return false;
    if (LocalStealthBlocksAim(local)) return false;
    return true;
}

} // namespace

namespace AutoAim {

void Install()
{
    // Install shoot hooks as soon as we have a local player — redirect is gated on IsAimRequested().
    if (GameState::GetLocalPtr() && !AimHooks::IsInstalled()) {
        if (!AimHooks::Install()) {
            static int s_n = 0;
            if ((s_n++ % 240) == 0)
                DBG_FILE_LOG("[AutoAim] AimHooks not installed yet (attempt=" << s_n << ")");
        }
    }

    if (!IsAimRequested())
        return;

    if (BootGate::FeatureAllowed("ProjectileTracking"))
        ProjectileTracking::RequestDeferredInstall();
    AoeTracking::Install();
}

void Uninstall()
{
    AimHooks::ReleaseAutoFire();
    AimHooks::Uninstall();
    s_hasTarget.store(false, std::memory_order_relaxed);
    s_aimFocusId.store(0, std::memory_order_relaxed);
    WeaponCalibrator::Reset();
}

void Tick()
{
    Install();

    // Always merge proxy wire enemies while in-world (independent of aim toggle).
    if (GameState::GetLocalPtr())
        SafeEnemyTrackerTick();

    const bool aimOn = IsAimRequested();
    AimHooks::SetAimActive(aimOn);

    const ULONGLONG wall = GetTickCount64();
    if (wall - s_lastThrottleMs < 8ULL) return;
    s_lastThrottleMs = wall;

    if (!aimOn) {
        s_hasTarget.store(false, std::memory_order_relaxed);
        s_aimFocusId.store(0, std::memory_order_relaxed);
        AimHooks::SetTarget(false, 0.f, 0.f);
        AimHooks::ReleaseAutoFire();
    } else if (!SafeInvokeRunTick()) {
        static ULONGLONG s_lastLogMs = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - s_lastLogMs > 2000ULL) {
            s_lastLogMs = now;
            DBG_FILE_LOG("[AutoAim] RunTick SEH phase=Outer — clearing target");
        }
        s_hasTarget.store(false, std::memory_order_relaxed);
        s_aimFocusId.store(0, std::memory_order_relaxed);
        AimHooks::SetTarget(false, 0.f, 0.f);
        AimHooks::ReleaseAutoFire();
    } else {
        void* local = GameState::GetLocalPtr();
        if (WantAutoFire(local))
            AimHooks::UpdateAutoFire(true);
        else
            AimHooks::ReleaseAutoFire();
    }

    static ULONGLONG s_lastHbMs = 0;
    if (wall - s_lastHbMs > 3000ULL) {
        s_lastHbMs = wall;
        const AimHooks::HookStats hs = AimHooks::GetHookStats();
        DBG_FILE_LOG("[AutoAim] hb aimOn=" << (aimOn ? 1 : 0)
            << " hooks=" << (AimHooks::IsInstalled() ? 1 : 0)
            << " enemies=" << EnemyTracker::GetLastSnapshotCount()
            << " pods=" << EnemyTracker::GetLastPodCount()
            << " target=" << (s_hasTarget.load(std::memory_order_relaxed) ? 1 : 0)
            << " focus=" << s_aimFocusId.load(std::memory_order_relaxed)
            << " autoFire=" << AimHooks::GetAutoFireShots()
            << " csa=" << hs.csaCalls << "/" << hs.csaRedirect
            << " swa=" << hs.swaCalls << "/" << hs.swaRedirect);
    }
}

void SetEnabled(bool on) {
    s_enabled.store(on, std::memory_order_relaxed);
    if (on) {
        Install();
    } else {
        s_hasTarget.store(false, std::memory_order_relaxed);
        s_aimFocusId.store(0, std::memory_order_relaxed);
        AimHooks::SetTarget(false, 0.f, 0.f);
        AimHooks::ReleaseAutoFire();
    }
}
bool IsEnabled() { return s_enabled.load(std::memory_order_relaxed); }

void SetAimMode(TargetSelector::Mode mode) {
    const int raw = static_cast<int>(mode);
    s_aimModeInt.store((raw < 0 || raw > 3) ? 0 : raw, std::memory_order_relaxed);
}
TargetSelector::Mode GetAimMode() {
    return static_cast<TargetSelector::Mode>(s_aimModeInt.load(std::memory_order_relaxed));
}

void SetLockTarget(int32_t enemyId) {
    s_lockedEnemyId.store(enemyId, std::memory_order_relaxed);
    s_aimModeInt.store(static_cast<int>(TargetSelector::Mode::Locked), std::memory_order_relaxed);
}

void SetShootInvulnerable(bool on)   { s_shootInvulnerable.store(on, std::memory_order_relaxed); }
bool IsShootInvulnerable()           { return s_shootInvulnerable.load(std::memory_order_relaxed); }

void SetPrioritizeBosses(bool on)    { s_prioritizeBosses.store(on, std::memory_order_relaxed); }
bool IsPrioritizeBosses()            { return s_prioritizeBosses.load(std::memory_order_relaxed); }

void SetIgnoreWalls(bool on)         { s_ignoreWalls.store(on, std::memory_order_relaxed); }
bool IsIgnoreWalls()                 { return s_ignoreWalls.load(std::memory_order_relaxed); }

void SetShootWhileStealthed(bool on) { s_shootWhileStealthed.store(on, std::memory_order_relaxed); }
bool IsShootWhileStealthed()         { return s_shootWhileStealthed.load(std::memory_order_relaxed); }

void SetPhaseSkipTypes(const int32_t* types, int count) {
    s_skipObjTypes = types;
    s_skipObjCount = count;
}

void SetMouseBoundingEnabled(bool on)  { s_mouseBoundingEnabled.store(on, std::memory_order_relaxed); }
bool IsMouseBoundingEnabled()          { return s_mouseBoundingEnabled.load(std::memory_order_relaxed); }
void SetMouseBoundingRange(float t)    {
    if (!std::isfinite(t) || t < 0.f) t = 0.f;
    if (t > 200.f) t = 200.f;
    s_mouseBoundingRange.store(t, std::memory_order_relaxed);
}
float GetMouseBoundingRange()          { return s_mouseBoundingRange.load(std::memory_order_relaxed); }

void SetRangeLeadBias(float t)         {
    if (!std::isfinite(t) || t < 0.f) t = 0.f;
    if (t > 50.f) t = 50.f;
    s_rangeLeadBias.store(t, std::memory_order_relaxed);
}
float GetRangeLeadBias()               { return s_rangeLeadBias.load(std::memory_order_relaxed); }

void SetReverseCultStaff(bool on)    { s_reverseCultStaff.store(on, std::memory_order_relaxed); AimHooks::SetReverseCultStaff(on); }
bool IsReverseCultStaff()            { return s_reverseCultStaff.load(std::memory_order_relaxed); }

void SetOffsetColossusSword(bool on) { s_offsetColossus.store(on, std::memory_order_relaxed); AimHooks::SetOffsetColossusSword(on); }
bool IsOffsetColossusSword()         { return s_offsetColossus.load(std::memory_order_relaxed); }

bool    HasTarget()       { return s_hasTarget.load(std::memory_order_relaxed); }
void    GetAimTarget(float& ox, float& oy) {
    ox = s_aimX.load(std::memory_order_relaxed);
    oy = s_aimY.load(std::memory_order_relaxed);
}
int32_t GetAimFocusEnemyId() { return s_aimFocusId.load(std::memory_order_relaxed); }

DiagView GetDiagView()
{
    const AimHooks::HookStats hs = AimHooks::GetHookStats();
    DiagView v{};
    v.aimRequested    = IsAimRequested();
    v.hooksInstalled  = AimHooks::IsInstalled();
    v.hasTarget       = s_hasTarget.load(std::memory_order_relaxed);
    v.enemyCount      = EnemyTracker::GetLastSnapshotCount();
    v.podCount        = EnemyTracker::GetLastPodCount();
    v.aimX            = s_aimX.load(std::memory_order_relaxed);
    v.aimY            = s_aimY.load(std::memory_order_relaxed);
    v.csaCalls        = hs.csaCalls;
    v.csaRedirect     = hs.csaRedirect;
    v.swaCalls        = hs.swaCalls;
    v.swaRedirect     = hs.swaRedirect;
    return v;
}

const WeaponProfile& GetWeaponProfile() { return WeaponCalibrator::GetProfile(); }

void OnLocalPlayerProjectileSpawn(void* projProps, bool isAbility,
                                   int32_t attackerObjId, uint32_t ownerObjId)
{
    if (isAbility || !projProps) return;
    if (!GameState::GetLocalPtr()) return;
    int32_t dk = ProjectileTracking::GetLocalPlayerObjectId();
    if (dk == 0) dk = EnemyTracker::GetLocalPlayerObjectId();
    if (dk == 0 || (attackerObjId != dk && static_cast<int32_t>(ownerObjId) != dk)) return;
    WeaponCalibrator::OnProjectileSpawn(projProps, GameState::GetLocalPtr());
}

void EnumerateLiveEnemies(EnemyScanCallback cb, void* user) {
    if (!cb) return;
    // Ensure a fresh snapshot — consumers (auto-dodge) may run with auto-aim off.
    // EnemyTracker::Tick is self-throttled, so this is deduped against the aim path.
    EnemyTracker::Tick();
    struct Ctx { EnemyScanCallback cb; void* user; };
    Ctx ctx{ cb, user };
    EnemyTracker::Enumerate([](const EnemyTracker::Entry& e, void* u) {
        auto* c = static_cast<Ctx*>(u);
        c->cb(e.x, e.y, e.id, c->user);
    }, &ctx);
}

} // namespace AutoAim
