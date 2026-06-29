#include "pch-il2cpp.h"

#include "ProjectileTracking.h"
#include "ProjectileCatalog.h"
#include "../../projectiles/ProjectileRuntimeReader.h"
#include "../../projectiles/ProjectileStore.h"
#include "../../projectiles/ProjectileTrajectory.h"
#include "AutoAim.h"
#include "FeatMagnetAim.h"
#include "gui/tabs/WorldTAB.h"
#include "helpers.h"
#include "Il2CppResolver.h"
#include "DbgFileLog.h"
#include "CrashTrace.h"
#include "BeebyteName.h"
#include "RuntimeOffsets.h"
#include "BootGate.h"

#include <windows.h>
#include "minhook/MinHook.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

// UI scale on native per-projectile mult (IL2CPP field KDAJOMOFMJB on HBEAKBIHANL).
static std::atomic<float> g_flashSpeedMulAtomic{1.f};

// Muzzle offset along aim (tiles). Default 0.3 = vanilla; hook skips trig when <= kMuzzleVanillaEps.
static std::atomic<float> g_localMuzzleOffsetTiles{0.3f};
static constexpr float kMuzzleMinTiles    = 0.3f;
static constexpr float kMuzzleMaxTiles    = 2.225f;
static constexpr float kMuzzleVanillaEps  = 0.00051f; // treat as disabled vs 0.3

namespace {
static const char* kProjClassName   = "HBEAKBIHANL";
static const char* kHbeakSpeedMulFieldName = "KDAJOMOFMJB"; // Flash speedMul_ equivalent (types.cs)
static const char* kSpawnMethodName = "KOBMINBDOBD";
static const int   kSpawnParamCount = 12;

// Projectile's BeeByte-obfuscated class changes per build; resolve it from
// readable metadata first, then fall back to the older hardcoded name.
__declspec(noinline) static Il2CppClass* ResolveProjClass()
{
    // Structural auto-recovery (A1) wins: the class found via its ProjectileProperties*
    // field survives BeeByte renames, so once the BootGate Discovery pass recovers it
    // we use it directly — bullets are captured again with no dump after a patch.
    if (Il2CppClass* rec = RuntimeOffsets::GetRecoveredProjClass()) return rec;

    static Il2CppClass* s_cached = nullptr;
    if (s_cached) return s_cached;

    const char* resolvedVia = nullptr;
    for (const auto& kv : Beebyte::GetMap()) {
        if (kv.second == "Projectile") {
            Il2CppClass* k = Resolver::GetClass("", kv.first.c_str());
            if (!k) k = Resolver::FindClassLoose(kv.first.c_str());
            if (k) { s_cached = k; resolvedVia = kv.first.c_str(); break; }
        }
    }
    if (!s_cached) {
        Il2CppClass* k = Resolver::GetClass("", kProjClassName);
        if (!k) k = Resolver::FindClassLoose(kProjClassName);
        if (k) { s_cached = k; resolvedVia = kProjClassName; }
    }

    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        if (s_cached)
            DBG_FILE_LOG("[ProjectileTracking] ResolveProjClass: resolved 'Projectile' via '"
                << (resolvedVia ? resolvedVia : "?")
                << "' (hardcoded was '" << kProjClassName << "')");
        else
            DBG_FILE_LOG("[ProjectileTracking] ResolveProjClass: FAILED — no 'Projectile' "
                "in Beebyte map and hardcoded '" << kProjClassName << "' unresolved");
    }
    return s_cached;
}
using SpawnProjectileFn = void* (__fastcall*)(
    void*    projInstance,
    void*    objProps,
    void*    projProps,
    int32_t  attackerObjId,
    uint32_t ownerObjId,
    float    angle,
    int32_t  bulletId,
    void*    name,
    void*    group,
    float    startX,
    float    startY,
    bool     canHitPlayer,
    bool     isAbility,
    void*    methodInfo);

SpawnProjectileFn         g_OriginalSpawn = nullptr;
CRITICAL_SECTION          g_EntCs;
std::atomic<int32_t>      g_LocalDictKey{ 0 };

std::unordered_map<int32_t, std::pair<float, float>> g_EntityPos;
bool                      g_Installed = false;
bool                      g_EntCsInit = false;

std::atomic<uint32_t>     g_hbeakSpeedMulFieldOff{ 0 }; // 0 = unresolved

static inline bool AddrOk(const void* p)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    return a > 0x10000 && a < 0x7FFFFFFFFFFFULL;
}

__declspec(noinline) static Il2CppClass* SafeResolveProjClass()
{
    Il2CppClass* klass = nullptr;
    __try {
        klass = ResolveProjClass();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        klass = nullptr;
    }
    return klass;
}

__declspec(noinline) static const MethodInfo* LookupSpawnMethod(Il2CppClass* klass)
{
    if (!klass) return nullptr;
    const MethodInfo* mi = il2cpp_class_get_method_from_name(klass, kSpawnMethodName, kSpawnParamCount);
    if (mi && AddrOk(mi->methodPointer)) return mi;
    void* iter = nullptr;
    for (const MethodInfo* cand; (cand = il2cpp_class_get_methods(klass, &iter)) != nullptr; ) {
        if (cand->parameters_count == kSpawnParamCount && AddrOk(cand->methodPointer))
            return cand;
    }
    return nullptr;
}

__declspec(noinline) static const MethodInfo* SafeLookupSpawnMethod(Il2CppClass* klass)
{
    const MethodInfo* mi = nullptr;
    __try {
        mi = LookupSpawnMethod(klass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mi = nullptr;
    }
    return mi;
}


static void EnsureHbeakSpeedMulFieldOffset()
{
    uint32_t cur = g_hbeakSpeedMulFieldOff.load(std::memory_order_relaxed);
    if (cur != 0) return;
    Il2CppClass* klass = ResolveProjClass();
    if (!klass) return;
    FieldInfo* fi = il2cpp_class_get_field_from_name(klass, kHbeakSpeedMulFieldName);
    if (!fi) return;
    const size_t off = il2cpp_field_get_offset(fi);
    if (off > 0u && off < 0x10000u)
        g_hbeakSpeedMulFieldOff.store(static_cast<uint32_t>(off), std::memory_order_relaxed);
}

static float ComputeEffectiveSpeedMulFromInstance(void* hbeakInstance)
{
    EnsureHbeakSpeedMulFieldOffset();
    float flashTune = ProjectileTracking::GetFlashSpeedMultiplier();
    if (!(flashTune > 0.01f) || flashTune > 50.f)
        flashTune = 1.f;

    float inst = 1.f;
    const uint32_t off = g_hbeakSpeedMulFieldOff.load(std::memory_order_relaxed);
    if (AddrOk(hbeakInstance) && off != 0u) {
        __try {
            float v = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(hbeakInstance) + off);
            if (std::isfinite(v) && v > 1e-6f && v < 100.f)
                inst = v;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    float p = inst * flashTune;
    if (!(p > 1e-6f) || p > 100.f)
        return 1.f;
    return p;
}

static bool TryReadLivePos(void* projInst, float& outX, float& outY)
{
    outX = 0.f;
    outY = 0.f;
    if (!AddrOk(projInst)) return false;
    __try {
        uint8_t* p = reinterpret_cast<uint8_t*>(projInst);
        outX = *reinterpret_cast<float*>(p + RuntimeOffsets::PosX);
        outY = *reinterpret_cast<float*>(p + RuntimeOffsets::PosY);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void LookupShooterOrigin(int32_t attackerObjId, uint32_t ownerObjId, float& originX, float& originY)
{
    EnterCriticalSection(&g_EntCs);
    auto itA = g_EntityPos.find(attackerObjId);
    if (itA != g_EntityPos.end()) {
        originX = itA->second.first;
        originY = itA->second.second;
        LeaveCriticalSection(&g_EntCs);
        return;
    }
    auto itO = g_EntityPos.find(static_cast<int32_t>(ownerObjId));
    if (itO != g_EntityPos.end()) {
        originX = itO->second.first;
        originY = itO->second.second;
        LeaveCriticalSection(&g_EntCs);
        return;
    }
    LeaveCriticalSection(&g_EntCs);
}

static bool TryReadObjectPropertiesIsEnemy(void* objProps, bool& outIsEnemy)
{
    outIsEnemy = false;
    if (!AddrOk(objProps)) return false;
    const uint32_t off = RuntimeOffsets::OP_IsEnemy;
    if (off == 0u || off >= 0x8000u) return false;
    __try {
        outIsEnemy = *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(objProps) + off);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static bool ProjectileOffsetsUsable()
{
    if (!BootGate::FeatureAllowed("ProjectileTracking")) return false;
    const uint32_t px = RuntimeOffsets::PosX;
    const uint32_t py = RuntimeOffsets::PosY;
    if (px < 0x8u || px >= 0x800u) return false;
    if (py < 0x8u || py >= 0x800u) return false;
    const uint32_t ppSpeed = RuntimeOffsets::PP_Speed;
    if (ppSpeed < 0x8u || ppSpeed >= 0x800u) return false;
    return true;
}

struct SpawnCaptureCtx {
    void*     ret;
    void*     objProps;
    void*     projProps;
    int32_t   attackerObjId;
    uint32_t  ownerObjId;
    float     angle;
    int32_t   bulletId;
    float     spawnX;
    float     spawnY;
    bool      canHitPlayer;
    bool      isLocalShot;
    ULONGLONG spawnTickPre;
};

// Inner SEH shell — no C++ objects with destructors in this function (C2712).
__declspec(noinline) static int SafeTrackSpawnedProjectile_SEH(const SpawnCaptureCtx* c)
{
    __try {
        bool ownerIsEnemy = false;
        const bool ownerClassified = TryReadObjectPropertiesIsEnemy(c->objProps, ownerIsEnemy);
        const bool isEnemyShot = !c->isLocalShot
            && ((ownerClassified && ownerIsEnemy) || (!ownerClassified && c->canHitPlayer));
        if (isEnemyShot) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "SpawnDetour:enemy shot oid=%u bid=%d",
                c->ownerObjId, c->bulletId);
            CrashTrace::Push(buf);
        }
        if (!c->isLocalShot && !isEnemyShot)
            return 1;

        float entityX = 0.f, entityY = 0.f;
        LookupShooterOrigin(c->attackerObjId, c->ownerObjId, entityX, entityY);

        float sx, sy;
        if (fabsf(entityX) > 0.5f || fabsf(entityY) > 0.5f) {
            sx = entityX + c->spawnX;
            sy = entityY + c->spawnY;
        } else {
            float liveX = 0.f, liveY = 0.f;
            if (TryReadLivePos(c->ret, liveX, liveY) &&
                (fabsf(liveX) > 0.5f || fabsf(liveY) > 0.5f)) {
                sx = liveX;
                sy = liveY;
            } else {
                sx = c->spawnX;
                sy = c->spawnY;
            }
        }

        WorldProjectile p{};
        p.startX = sx;
        p.startY = sy;
        p.angle = c->angle;
        p.spawnTick = c->spawnTickPre;
        p.valid = true;
        p.canHitPlayer = c->canHitPlayer;
        p.ptr = c->ret;
        p.bulletId = c->bulletId;
        p.attackerObjId = c->attackerObjId;
        p.ownerObjId = c->ownerObjId;
        p.speed = 5000.f;
        p.lifetime = 2000.f;
        p.minDamage = 100;
        p.damage = 100;
        p.x = sx;
        p.y = sy;

        void* const ppEffective = ProjectileRuntimeReader::EffectivePropsFromProjectile(c->ret, c->projProps);
        if (ProjectileRuntimeReader::ApplyProperties(p, c->ret, ppEffective,
                ProjectileCollisionFallback::SpawnHook)) {
            if (p.speed < 1.f || p.speed > 50000.f || p.lifetime < 50.f || p.lifetime > 600000.f) {
                p.speed = 5000.f;
                p.lifetime = 2000.f;
                p.minDamage = 100;
                p.damage = 100;
            }
        }

        p.speedMul = ComputeEffectiveSpeedMulFromInstance(c->ret);

        if (AddrOk(c->ret)) {
            const uint32_t px = RuntimeOffsets::PosX;
            const uint32_t py = RuntimeOffsets::PosY;
            if (px >= 0x8u && px < 0x800u && py >= 0x8u && py < 0x800u) {
                uint8_t* pi = reinterpret_cast<uint8_t*>(c->ret);
                const float rx = *reinterpret_cast<float*>(pi + px);
                const float ry = *reinterpret_cast<float*>(pi + py);
                if (std::isfinite(rx) && std::isfinite(ry)) {
                    p.x = rx;
                    p.y = ry;
                }
            }
        }
        if (!(fabsf(p.x) > 0.5f || fabsf(p.y) > 0.5f)) {
            float posX = p.x;
            float posY = p.y;
            if (ProjectileTrajectory::GetPositionAtTime(p, 0.f, posX, posY)) {
                p.x = posX;
                p.y = posY;
            }
        }

        ProjectileTrajectory::CachePath(p);

        const WorldProjectile snap = ProjectileStore::StoreProjectile(isEnemyShot, p);
        if (isEnemyShot) {
            ProjectileCatalog::RecordSpawn(0, snap);
            ProjectileStore::NotifyHazardSpawn(snap);
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void SafeTrackSpawnedProjectile(const SpawnCaptureCtx& c)
{
    if (SafeTrackSpawnedProjectile_SEH(&c)) return;
    static int s_sehN = 0;
    if (s_sehN++ < 8)
        DBG_FILE_LOG("[ProjectileTracking] SpawnDetour track SEH (enemy/local bullet capture aborted)");
    CrashTrace::Push("SpawnDetour:SEH track");
}

void* __fastcall SpawnProjectileDetour(
    void*    projInstance,
    void*    objProps,
    void*    projProps,
    int32_t  attackerObjId,
    uint32_t ownerObjId,
    float    angle,
    int32_t  bulletId,
    void*    name,
    void*    group,
    float    startX,
    float    startY,
    bool     canHitPlayer,
    bool     isAbility,
    void*    methodInfo)
{
    // Never call EnsureAll / IL2CPP walks from the spawn hook thread — caused
    // illegal-instruction crashes when the first enemy bullet fired (~tid != render).
    float spawnX = startX;
    float spawnY = startY;
    const int32_t dk = g_LocalDictKey.load(std::memory_order_relaxed);
    const bool isLocalShot = dk != 0 && (attackerObjId == dk || static_cast<int32_t>(ownerObjId) == dk);
    if (isLocalShot && CombatTAB::FeatMagnetAim::IsEnabled()) {
        const float magnetTiles = CombatTAB::FeatMagnetAim::GetVisualOffsetTiles();
        bool useTarget = false;
        if (AutoAim::HasTarget()) {
            float targetX = 0.f, targetY = 0.f;
            AutoAim::GetAimTarget(targetX, targetY);

            float entityX = 0.f, entityY = 0.f;
            LookupShooterOrigin(attackerObjId, ownerObjId, entityX, entityY);
            if (fabsf(entityX) > 0.5f || fabsf(entityY) > 0.5f) {
                const float dx = targetX - entityX;
                const float dy = targetY - entityY;
                const float lenSq = dx * dx + dy * dy;
                if (lenSq > 1e-6f) {
                    const float invLen = 1.f / sqrtf(lenSq);
                    spawnX = dx * invLen * magnetTiles;
                    spawnY = dy * invLen * magnetTiles;
                    useTarget = true;
                }
            }
        }
        if (!useTarget) {
            spawnX = cosf(angle) * magnetTiles;
            spawnY = sinf(angle) * magnetTiles;
        }
    } else {
        const float muzzleTiles = g_localMuzzleOffsetTiles.load(std::memory_order_relaxed);
        if (muzzleTiles > kMuzzleMinTiles + kMuzzleVanillaEps && isLocalShot) {
            // startX/startY are shooter-relative; vanilla length ~0.3 tiles. Scale to keep direction.
            const float scale = muzzleTiles / kMuzzleMinTiles;
            if (fabsf(startX) > 1e-5f || fabsf(startY) > 1e-5f) {
                spawnX = startX * scale;
                spawnY = startY * scale;
            } else {
                spawnX = cosf(angle) * muzzleTiles;
                spawnY = sinf(angle) * muzzleTiles;
            }
        }
    }

    AutoAim::OnLocalPlayerProjectileSpawn(projProps, isAbility, attackerObjId, ownerObjId);

    // Capture spawn timestamp BEFORE the original spawn runs. The IL2CPP method does
    // allocations / virtual dispatch and can take 0.2-2 ms; if we capture spawnTick
    // afterward (and after our own LookupShooterOrigin / live-pos read / CS enter)
    // every prediction is biased late by that amount, manifesting as "bullets arrive
    // earlier than predicted" -> chip damage.
    const ULONGLONG spawnTickPre = GetTickCount64();

    // Call game first: HBEAKBIHANL_KOBMINBDOBD returns the live projectile instance.
    // The first argument is not reliably that instance (factory/this); using it for X/Y was wrong.
    void* ret = g_OriginalSpawn(
        projInstance, objProps, projProps, attackerObjId, ownerObjId, angle, bulletId,
        name, group, spawnX, spawnY, canHitPlayer, isAbility, methodInfo);

    if (!AddrOk(ret))
        return ret;

    if (ProjectileOffsetsUsable()) {
        const SpawnCaptureCtx ctx{
            ret, objProps, projProps, attackerObjId, ownerObjId,
            angle, bulletId, spawnX, spawnY, canHitPlayer, isLocalShot, spawnTickPre };
        SafeTrackSpawnedProjectile(ctx);
    }

    return ret;
}

} // namespace

namespace ProjectileTracking {

static void* g_spawnTarget = nullptr;
static int   s_deferInstallFrames = 0;
constexpr int kProjInstallDeferFramesMenu = 480;
constexpr int kProjInstallDeferFramesRealm = 30;

void RequestDeferredInstall()
{
    if (g_Installed) return;
    s_deferInstallFrames = BootGate::LazyOffsetLookupAllowed()
        ? kProjInstallDeferFramesRealm
        : kProjInstallDeferFramesMenu;
}

void TickDeferredInstall()
{
    if (g_Installed) return;
    if (s_deferInstallFrames > 0) {
        --s_deferInstallFrames;
        return;
    }
    if (!BootGate::FeatureAllowed("ProjectileTracking"))
        return;
    Install();
}

void Install()
{
    if (g_Installed) return;
    if (!BootGate::FeatureAllowed("ProjectileTracking")) {
        static int s_n = 0;
        if ((s_n++ % 240) == 0)
            DBG_FILE_LOG("[ProjectileTracking] Install blocked: BootGate offsets not ready "
                "(HBEAKBIHANL or deps stale, attempt=" << s_n << ")");
        return;
    }
    {
        static int s_n = 0;
        if ((s_n++ % 240) == 0)
            DBG_FILE_LOG("[ProjectileTracking] Install() reached, not yet installed "
                "(attempt=" << s_n << ") — resolving hook target...");
    }
    ProjectileStore::Initialize();
    if (!g_EntCsInit) {
        InitializeCriticalSection(&g_EntCs);
        g_EntCsInit = true;
    }

    Il2CppClass* klass = SafeResolveProjClass();
    if (!klass || !AddrOk(klass)) {
        static int s_n = 0;
        if ((s_n++ % 240) == 0)
            DBG_FILE_LOG("[ProjectileTracking] Install: projectile class UNRESOLVED "
                "(attempt=" << s_n << ")");
        return;
    }
    const MethodInfo* mi = SafeLookupSpawnMethod(klass);
    if (!mi || !AddrOk(mi->methodPointer)) {
        static int s_n = 0;
        if ((s_n++ % 240) == 0)
            DBG_FILE_LOG("[ProjectileTracking] Install: class OK but spawn method '"
                << kSpawnMethodName << "'(" << kSpawnParamCount << " args) UNRESOLVED "
                "— BeeByte method name/arity stale. (attempt=" << s_n << ")");
        return;
    }

    g_spawnTarget = reinterpret_cast<void*>(mi->methodPointer);
    g_OriginalSpawn = reinterpret_cast<SpawnProjectileFn>(g_spawnTarget);

    static bool s_mhInit = false;
    if (!s_mhInit) {
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return;
        s_mhInit = true;
    }

    if (MH_CreateHook(g_spawnTarget,
            reinterpret_cast<void*>(&SpawnProjectileDetour),
            reinterpret_cast<void**>(&g_OriginalSpawn)) != MH_OK)
        return;
    // Queue + ApplyQueued freezes threads — safe when structural scan recovers the
    // projectile class mid-combat (MH_EnableHook during active spawns crashed).
    if (MH_QueueEnableHook(g_spawnTarget) != MH_OK)
        return;
    if (MH_ApplyQueued() != MH_OK)
        return;

    EnsureHbeakSpeedMulFieldOffset();
    g_Installed = true;
    CrashTrace::Push("ProjectileTracking:spawn hook INSTALLED");
    DBG_FILE_LOG("[ProjectileTracking] Install: spawn hook INSTALLED — bullets now captured");
}

bool IsInstalled()
{
    return g_Installed;
}

void Uninstall()
{
    if (g_Installed) {
        if (g_spawnTarget) {
            MH_DisableHook(g_spawnTarget);
            MH_RemoveHook(g_spawnTarget);
        }
        g_OriginalSpawn = nullptr;
        g_spawnTarget = nullptr;
        g_Installed = false;
    }
    ProjectileStore::Shutdown();
    if (g_EntCsInit) {
        DeleteCriticalSection(&g_EntCs);
        g_EntCsInit = false;
    }
}

void ComputePosAtSafe(const WorldProjectile& proj, float tMs, float& outX, float& outY)
{
    const float fallbackX = outX;
    const float fallbackY = outY;
    if (!ProjectileTrajectory::GetPositionAtTime(proj, tMs, outX, outY)) {
        outX = fallbackX;
        outY = fallbackY;
    }
}

void SetLocalPlayerObjectId(int32_t objectId)
{
    g_LocalDictKey.store(objectId, std::memory_order_relaxed);
}

int32_t GetLocalPlayerObjectId()
{
    return g_LocalDictKey.load(std::memory_order_relaxed);
}

void OnWorldRefreshBegin()
{
    if (!g_EntCsInit) return;
    EnterCriticalSection(&g_EntCs);
    g_EntityPos.clear();
    LeaveCriticalSection(&g_EntCs);
}

void OnWorldEntity(int32_t objectId, float x, float y)
{
    if (!g_EntCsInit) return;
    EnterCriticalSection(&g_EntCs);
    g_EntityPos[objectId] = { x, y };
    LeaveCriticalSection(&g_EntCs);
}

void SnapshotToWorld(std::vector<WorldProjectile>& out)
{
    ProjectileStore::SnapshotToWorld(out);
}

void CopyActiveForDraw(std::vector<WorldProjectile>& out)
{
    ProjectileStore::CopyActiveForDraw(out);
}

int CountValidForDiagnostics()
{
    return ProjectileStore::CountValidForDiagnostics();
}

void CopyActiveLocalForDraw(std::vector<WorldProjectile>& out)
{
    ProjectileStore::CopyActiveLocalForDraw(out);
}

void ComputePosAt(const WorldProjectile& proj, float tMs, float& outX, float& outY)
{
    const float fallbackX = outX;
    const float fallbackY = outY;
    if (!ProjectileTrajectory::GetPositionAtTime(proj, tMs, outX, outY)) {
        outX = fallbackX;
        outY = fallbackY;
    }
}

void SetFlashSpeedMultiplier(float m)
{
    float c = m;
    if (!(c > 0.01f) || c > 50.f)
        c = 1.f;
    g_flashSpeedMulAtomic.store(c, std::memory_order_relaxed);
}

float GetFlashSpeedMultiplier()
{
    return g_flashSpeedMulAtomic.load(std::memory_order_relaxed);
}

void SetLocalPlayerMuzzleOffsetTiles(float tiles)
{
    float v = tiles;
    if (v < kMuzzleMinTiles) v = kMuzzleMinTiles;
    if (v > kMuzzleMaxTiles) v = kMuzzleMaxTiles;
    g_localMuzzleOffsetTiles.store(v, std::memory_order_relaxed);
}

float GetLocalPlayerMuzzleOffsetTiles()
{
    return g_localMuzzleOffsetTiles.load(std::memory_order_relaxed);
}

float EffectiveSpeedMulFromProjectile(void* hbeakInstance)
{
    return ComputeEffectiveSpeedMulFromInstance(hbeakInstance);
}

float NormalizeProjectileLifetimeMs(float rawFromProps)
{
    return ProjectileTrajectory::NormalizeLifetimeMs(rawFromProps);
}

float NormalizeAccelDelayMs(float rawFromProps)
{
    return ProjectileTrajectory::NormalizeAccelDelayMs(rawFromProps);
}

bool TryReadProjRadiusFromInstance(void* hbeakInstance, float& outRadius)
{
    outRadius = 0.f;
    if (!hbeakInstance) return false;
    __try {
        outRadius = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(hbeakInstance) + RuntimeOffsets::Hbeak_ProjRadius);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

uint32_t GetHbeakProjRadiusOffset()
{
    return RuntimeOffsets::Hbeak_ProjRadius;
}

void RegisterHazardSpawnCallback(HazardSpawnCb cb, void* user)
{
    ProjectileStore::RegisterHazardSpawnCallback(cb, user);
}

void ClearHazardSpawnCallback()
{
    ProjectileStore::ClearHazardSpawnCallback();
}

} // namespace ProjectileTracking
