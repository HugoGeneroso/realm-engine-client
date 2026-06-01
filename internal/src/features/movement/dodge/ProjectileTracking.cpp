#include "pch-il2cpp.h"

#include "ProjectileTracking.h"
#include "ProjectileCatalog.h"
#include "AutoAim.h"
#include "FeatMagnetAim.h"
#include "gui/tabs/WorldTAB.h"
#include "helpers.h"
#include "Il2CppResolver.h"
#include "DbgFileLog.h"
#include "BeebyteName.h"
#include "RuntimeOffsets.h"

#include <windows.h>
#include "minhook/MinHook.h"

#include <atomic>
#include <cmath>
#include <cstdio>
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

// ProjectileProperties.Lifetime: usually seconds in XML; already-ms values are typically >= ~250.
static float NormalizeLifetimeToMs(float rawFromProps)
{
    if (!(rawFromProps > 0.f) || rawFromProps != rawFromProps)
        return 2000.f;
    if (rawFromProps < 250.f)
        return rawFromProps * 1000.f;
    return rawFromProps;
}

// AccelDelay from ProjectileProperties: assembly confirms it is always a float in seconds,
// compared against elapsed-time/1000 (i.e., t_s). Same heuristic threshold as Lifetime.
static float NormalizeAccelDelayToMs(float raw)
{
    if (!(raw > 0.f) || raw != raw)
        return 0.f;
    if (raw < 250.f)          // < 250 → stored in seconds; >= 250 → already ms
        return raw * 1000.f;
    return raw;
}

namespace {

// SpawnProjectileDetour uses SEH (__try). DBG_FILE_LOG builds a std::ostringstream
// (object requiring unwinding), which MSVC forbids in the same function as __try
// (C2712). Isolate the logging in noinline helpers — same pattern as
// AutoAim::AutoAimLogRunning.
__declspec(noinline) static void PtLogDetourFired(int count, int bulletId,
                                                  int32_t attacker, uint32_t owner)
{
    DBG_FILE_LOG("[ProjectileTracking] detour FIRED (count=" << count
        << " bulletId=" << bulletId << " attacker=" << attacker
        << " owner=" << owner << ")");
}
__declspec(noinline) static void PtLogStored(uint32_t idx, int enemy,
                                              float sx, float sy, int n)
{
    DBG_FILE_LOG("[ProjectileTracking] stored slot idx=" << idx
        << " enemy=" << enemy << " pos=(" << sx << "," << sy << ")"
        << " (storeCount=" << n << ")");
}

constexpr int           kMaxTrackedProj     = 256;

static const char* kProjClassName   = "HBEAKBIHANL";
static const char* kHbeakSpeedMulFieldName = "KDAJOMOFMJB"; // Flash speedMul_ equivalent (types.cs)
static const char* kSpawnMethodName = "KOBMINBDOBD";
static const int   kSpawnParamCount = 12;

// The projectile class's BeeByte-obfuscated name changes every game update.
// kProjClassName ("HBEAKBIHANL") is stale on the current build — proven by
// dll-trace ("projectile class 'HBEAKBIHANL' UNRESOLVED"). BeebyteName.h is
// auto-regenerated per build and maps obfuscated->readable; reverse-scan it
// for the class whose readable name is "Projectile" to get the CURRENT
// obfuscated name, then resolve. Falls back to the hardcoded name (keeps
// older game builds / b0's setup working). Self-corrects on future updates.
__declspec(noinline) static Il2CppClass* ResolveProjClass()
{
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
constexpr float         kProjVisualTimeOffsetMs = 0.f;
static const uint32_t& kOffWorldX = RuntimeOffsets::PosX;
static const uint32_t& kOffWorldY = RuntimeOffsets::PosY;
// HBEAKBIHANL.HHFDCMIIIHF (projRadius) — offset from RuntimeOffsets::Hbeak_ProjRadius (IL2CPP + BeeByte).

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
CRITICAL_SECTION          g_RingCs;
CRITICAL_SECTION          g_EntCs;
std::atomic<int32_t>      g_LocalDictKey{ 0 };
std::atomic<float>        g_MuzzleDbgSpawnX{ 0.f };
std::atomic<float>        g_MuzzleDbgSpawnY{ 0.f };
std::atomic<int>          g_MuzzleDbgHasSpawn{ 0 };
std::atomic<bool>         g_ShowPaths{ false };
std::atomic<bool>         g_ShowHitboxes{ false };
std::atomic<uint32_t>     g_WriteIdx{ 0 };
WorldProjectile           g_Slots[kMaxTrackedProj]{};

constexpr int             kMaxLocalProj = 64;
CRITICAL_SECTION          g_LocalCs;
std::atomic<uint32_t>     g_LocalWriteIdx{ 0 };
WorldProjectile           g_LocalSlots[kMaxLocalProj]{};
bool                      g_LocalCsInit = false;

std::unordered_map<int32_t, std::pair<float, float>> g_EntityPos;
bool                      g_Installed = false;
bool                      g_CsInit = false;

// Hazard-spawn callback (DangerPlanner consumer). Stored behind g_RingCs.
ProjectileTracking::HazardSpawnCb g_HazardCb = nullptr;
void*                             g_HazardCbUser = nullptr;

std::atomic<uint32_t>     g_hbeakSpeedMulFieldOff{ 0 }; // 0 = unresolved

static inline bool AddrOk(const void* p)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    return a > 0x10000 && a < 0x7FFFFFFFFFFFULL;
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

// Linear accel from ProjectileProperties floats only (no motion heuristics).
static float EffectiveProjectileLinearAccel(const WorldProjectile& proj)
{
    if (fabsf(proj.acceleration) > 1e-6f)
        return proj.acceleration;
    if (fabsf(proj.accelerationInv) > 1e-12f && std::isfinite(proj.accelerationInv))
        return 1.f / proj.accelerationInv;
    if (fabsf(proj.velocityChangeRate) > 1e-6f)
        return proj.velocityChangeRate;
    if (fabsf(proj.velocityChangeRateInv) > 1e-12f && std::isfinite(proj.velocityChangeRateInv))
        return 1.f / proj.velocityChangeRateInv;
    return 0.f;
}

// Accel used along the aim axis: props-derived, else implicit symmetric boomerang decel when XML omits Acceleration*.
// (-2*speed/lifetime in raw speed units matches v→0 at t=lifetime/2 when integrated with /10000 scaling.)
//
// Boolean gate: the game applies acceleration only when BOTH IsAccelerating
// (this type CAN accelerate) AND UseAcceleration (this shot DOES) are true.
// Boomerangs are the exception — they use implicit decel even with the
// flags off (the XML omits Acceleration*), preserved below.
static float ResolvedLinearAccelForDistance(const WorldProjectile& proj)
{
    if (!proj.boomerang && !(proj.isAccelerating && proj.useAccel))
        return 0.f;
    float a = EffectiveProjectileLinearAccel(proj);
    if (fabsf(a) <= 1e-6f && proj.boomerang
        && proj.lifetime > 1e-3f && proj.speed > 1.f && proj.speed <= 50000.f)
        a = -2.f * proj.speed / proj.lifetime;
    return a;
}

// True when ResolvedLinearAccelForDistance is synthesizing the implicit
// boomerang decel (XML omits Acceleration*). In that case the kinematic
// integral naturally produces boomerang motion — apex at life/2, back to 0
// at life — IF we skip the v=0 clamp inside IntegratedDistanceAlongAim AND
// skip the explicit fold in ProjPosAt. Layering both on a clamped integral
// double-counts the deceleration: at t=life/2 the fold returns 0 instead of
// the apex distance, so the predictor places the bullet back at the spawn
// origin during its peak — and the planner walks straight into it.
static bool UsingImplicitBoomerangDecel(const WorldProjectile& proj)
{
    if (!proj.boomerang) return false;
    if (fabsf(EffectiveProjectileLinearAccel(proj)) > 1e-6f) return false;
    return (proj.lifetime > 1e-3f && proj.speed > 1.f && proj.speed <= 50000.f);
}

// Signed distance along the aim axis from t=0 to t=tMs (ms), with optional constant acceleration,
// delay before accel applies, and SpeedClamp (max speed when accelerating, min speed when decelerating).
static float IntegratedDistanceAlongAim(const WorldProjectile& proj, float tMs)
{
    if (tMs <= 0.f)
        return 0.f;

    const float speedMul = (proj.speedMul > 1e-6f && proj.speedMul < 100.f) ? proj.speedMul : 1.f;
    const float baseSpeedTpMs = (proj.speed / 10000.f) * speedMul;

    const float accelLinear = ResolvedLinearAccelForDistance(proj);
    const bool useKinematic = fabsf(accelLinear) > 1e-6f;
    if (!useKinematic)
        return tMs * baseSpeedTpMs;

    // Acceleration is a float in tiles/s² (assembly: movss, no divisor applied).
    // Converting to tiles/ms² requires dividing by 1000² = 1,000,000.
    // SpeedClamp is a float in tiles/s (same post-norm units as Speed/10).
    // Converting to tiles/ms requires dividing by 1,000.
    // AccelDelay is already normalised to ms by NormalizeAccelDelayToMs.
    // Assembly also scales delay by 1/speedMul; approximate here.
    //
    // speedMul² on acceleration: speedMul time-dilates the bullet's motion, i.e.
    // the bullet behaves as if real time t were replaced by t·N (N = speedMul).
    // Then v₀ scales by N and a scales by N² because pos = v₀·N·t + ½·a·N²·t².
    // The previous single-N scaling underscaled accel for shots with non-1.0
    // speedMul, making accelerating shots predict too-flat trajectories.
    const float accelTpMs2 = (accelLinear / 1000000.f) * speedMul * speedMul;
    const float speedTpMs = baseSpeedTpMs;
    const float delay = (proj.accelDelay > 0.f) ? proj.accelDelay / speedMul : 0.f;

    if (tMs <= delay)
        return tMs * speedTpMs;

    const float d1 = delay * speedTpMs;
    const float t2 = tMs - delay;
    float d2 = speedTpMs * t2 + 0.5f * accelTpMs2 * t2 * t2;

    if (proj.speedClamp > 0.f) {
        const float clampTpMs = (proj.speedClamp / 1000.f) * speedMul;
        if (accelTpMs2 > 1e-12f && clampTpMs > speedTpMs) {
            const float tClamp = (clampTpMs - speedTpMs) / accelTpMs2;
            if (tClamp > 0.f && t2 > tClamp) {
                const float dPre = speedTpMs * tClamp + 0.5f * accelTpMs2 * tClamp * tClamp;
                d2 = dPre + clampTpMs * (t2 - tClamp);
            }
        } else if (accelTpMs2 < -1e-12f && clampTpMs < speedTpMs && clampTpMs >= 0.f) {
            const float tFloor = (speedTpMs - clampTpMs) / (-accelTpMs2);
            if (tFloor > 0.f && t2 > tFloor) {
                const float dToFloor = speedTpMs * tFloor + 0.5f * accelTpMs2 * tFloor * tFloor;
                d2 = dToFloor + clampTpMs * (t2 - tFloor);
            }
        }
    } else if (accelTpMs2 < 0.f && !UsingImplicitBoomerangDecel(proj)) {
        // Decelerating shots stop at v=0; boomerangs reverse past v=0 (handled
        // by the unclamped integral when implicit decel is active).
        const float tStop = speedTpMs / (-accelTpMs2);
        if (t2 > tStop)
            d2 = speedTpMs * tStop + 0.5f * accelTpMs2 * tStop * tStop;
    }

    return d1 + d2;
}

// Matches Flash/Unity projectile motion along aim (with Exalt accel / clamp extensions).
// Branch priority matches binary positionAt: Laser → IsWavy → IsParametric → IsTurning/isCircleTurnDelayed → linear.
static void ProjPosAt(const WorldProjectile& proj, float tMs, float& outX, float& outY)
{
    static constexpr float kPI = 3.14159265358979323846f;

    const float distance = IntegratedDistanceAlongAim(proj, tMs);

    // Laser: BJLDGDKMPFL::positionAt always returns spawn origin regardless of time.
    // The beam is a static capsule — the projectile instance never translates.
    if (proj.laser && proj.laserDistance > 1e-4f) {
        outX = proj.startX;
        outY = proj.startY;
        return;
    }

    if (proj.wavy) {
        const float phaseParity = ((proj.bulletId % 2) == 0) ? 0.f : kPI;
        if (proj.hasCustomAmplitude && proj.amplitude != 0.f) {
            // Custom-amplitude wavy: lateral perpendicular offset using per-projectile
            // Amplitude and Frequency fields. Same formula as the amplitude path below,
            // just triggered by IsWavy + HasCustomAmplitude. Assembly: [ProjProps+0x1A0] != 0.
            const float life = (proj.lifetime > 0.001f) ? proj.lifetime : 1.f;
            const float lateral = proj.amplitude
                * sinf(phaseParity + ((tMs / life) * proj.frequency * 2.f * kPI));
            outX = proj.startX + distance * cosf(proj.angle) + lateral * cosf(proj.angle + kPI * 0.5f);
            outY = proj.startY + distance * sinf(proj.angle) + lateral * sinf(proj.angle + kPI * 0.5f);
        } else {
            // Standard wavy: angle modulation with hardcoded Flash constants (π/64 amp, 6π/s freq).
            const float effAngle = proj.angle + (kPI / 64.f)
                * sinf(phaseParity + ((6.f * kPI * tMs) / 1000.f));
            outX = proj.startX + distance * cosf(effAngle);
            outY = proj.startY + distance * sinf(effAngle);
        }
        return;
    }

    if (proj.parametric) {
        const float life = (proj.lifetime > 0.001f) ? proj.lifetime : 1.f;
        const float ang8 = (tMs / life) * 2.f * kPI;
        const float s1 = sinf(ang8) * (((proj.bulletId % 2) != 0) ? 1.f : -1.f);
        const float s2 = sinf(2.f * ang8) * ((((proj.bulletId % 4) + 4) % 4) < 2 ? 1.f : -1.f);
        const float sa = sinf(proj.angle), ca = cosf(proj.angle);
        const float mag = proj.magnitude;
        outX = proj.startX + ((s1 * ca) - (s2 * sa)) * mag;
        outY = proj.startY + ((s1 * sa) + (s2 * ca)) * mag;
        return;
    }

    // ── IsTurning / isCircleTurnDelayed: polar / spiral path ──
    //
    // Binary priority: after IsWavy and IsParametric. isCircleTurnDelayed enters
    // this branch even when IsTurning=0 (verified in positionAt disassembly at 0x181101986).
    //
    // Non-circled (standard turning):
    //   pos = origin + distance(t) × (cos(angle + turnAngle(t)), sin(angle + turnAngle(t)))
    //
    // Circled (isCircleTurnDelayed):
    //   Pre-delay : straight line — pos = origin + kinematicDist(t) × direction
    //   Post-delay: fixed orbit  — pos = origin + orbitRadius × (cos(angle + omega*(t−delay)), sin(...))
    //   orbitRadius = kinematicDist(circleTurnDelay)  (confirmed: binary calls CHBDHBNBAOL() for this)
    if ((proj.isTurning || proj.isCircleTurnDelayed) && proj.turnStopTime > 1.f) {
        const float baseSpdTpMs = proj.speed / 10000.f * proj.speedMul;
        const bool  useCircle = proj.isCircleTurnDelayed && fabsf(proj.circleTurnAngle) > 1e-8f;
        const float effRate   = useCircle ? proj.circleTurnAngle : proj.turnRate;

        if (fabsf(effRate) > 1e-8f) {
            // Exalt XML semantics:
            //   TurnRate (proj.turnRate)        — radians per 50 ms server tick
            //   CircleTurnAngle (proj.circleTurnAngle) — total arc swept over turnStopTime ms
            const float omega = useCircle
                ? (proj.circleTurnAngle / proj.turnStopTime)
                : (proj.turnRate / 50.f);                              // rad/ms
            const float delayMs = useCircle ? proj.circleTurnDelay
                                            : (proj.isTurningDelayed ? proj.turnRateDelay : 0.f);

            const float dtArc = (tMs > delayMs) ? (tMs - delayMs) : 0.f;

            // Circle pre-delay: bullet travels straight to the orbit entry point.
            // Binary: xmm8 stays as kinematicDist(tMs) when time < circleTurnDelay.
            if (useCircle && dtArc <= 0.f) {
                outX = proj.startX + distance * cosf(proj.angle);
                outY = proj.startY + distance * sinf(proj.angle);
                return;
            }

            // Raw turn angle from constant angular velocity.
            float turnAngle = omega * dtArc;

            // Boomerang / accelerated-turn modifier (JOMFDAHJELJEff).
            // When IsTurningAccelerated=0, this is a pass-through (no-op).
            if (proj.isTurningAccelerated && dtArc > 0.f) {
                const float tSec = dtArc / 1000.f;
                if (tSec >= proj.turnAccelDelay && proj.turnAccelDelay >= 0.f) {
                    const float dt = tSec - proj.turnAccelDelay;
                    if (proj.turnAccelInv > 0.f) {
                        const float clamp = fmaxf(proj.turnClamp, proj.turnRate) - proj.turnRate;
                        const float threshold = clamp * proj.turnAccelInv;
                        if (dt <= threshold)
                            turnAngle += 0.5f * proj.turnAcceleration * dt * dt;
                        else
                            turnAngle += 0.5f * threshold * clamp + (dt - threshold) * clamp;
                    } else if (proj.turnAccelInv < 0.f) {
                        const float clamp = fminf(proj.turnClamp, proj.turnRate) - proj.turnRate;
                        const float threshold = clamp * proj.turnAccelInv;
                        if (dt <= threshold)
                            turnAngle += 0.5f * proj.turnAcceleration * dt * dt;
                        else
                            turnAngle += 0.5f * threshold * clamp + (dt - threshold) * clamp;
                    }
                }
            }

            // TurnStopTime cap: game's TurnAngleAtTime returns 0 after TurnStopTime
            // for the standard positionAt variant (forceAccel=false).
            const bool pastTurnStop = proj.turnSnapsToStraight && dtArc > proj.turnStopTime;
            if (pastTurnStop) {
                // Compute the arc endpoint and continue straight from there.
                const float capAngle = omega * proj.turnStopTime;
                const float thetaCap = proj.angle + capAngle;
                if (useCircle) {
                    const float circR = IntegratedDistanceAlongAim(proj, delayMs);
                    const float arcEndX = proj.startX + circR * cosf(thetaCap);
                    const float arcEndY = proj.startY + circR * sinf(thetaCap);
                    const float extraMs = dtArc - proj.turnStopTime;
                    outX = arcEndX + extraMs * baseSpdTpMs * cosf(thetaCap);
                    outY = arcEndY + extraMs * baseSpdTpMs * sinf(thetaCap);
                } else {
                    const float distCap = IntegratedDistanceAlongAim(proj, delayMs + proj.turnStopTime);
                    const float arcEndX = proj.startX + distCap * cosf(thetaCap);
                    const float arcEndY = proj.startY + distCap * sinf(thetaCap);
                    const float extraMs = dtArc - proj.turnStopTime;
                    outX = arcEndX + extraMs * baseSpdTpMs * cosf(thetaCap);
                    outY = arcEndY + extraMs * baseSpdTpMs * sinf(thetaCap);
                }
                return;
            }

            const float theta = proj.angle + turnAngle;
            if (useCircle) {
                // Fixed-radius orbit: radius = distance traveled during the straight delay phase.
                const float circR = IntegratedDistanceAlongAim(proj, delayMs);
                outX = proj.startX + circR * cosf(theta);
                outY = proj.startY + circR * sinf(theta);
            } else {
                // Spiral: distance grows while angle rotates.
                const float dist = IntegratedDistanceAlongAim(proj, tMs);
                outX = proj.startX + dist * cosf(theta);
                outY = proj.startY + dist * sinf(theta);
            }
            return;
        }
    }

    float foldedDistance = distance;
    // Only fold for boomerangs that have an *explicit* XML-provided accel —
    // those still travel forward then return via the mirror trick. Boomerangs
    // using implicit symmetric decel reverse on their own through the
    // unclamped kinematic integral; folding them double-counts and predicts
    // the bullet back at origin during its apex.
    if (proj.boomerang && !UsingImplicitBoomerangDecel(proj)) {
        const float life = (proj.lifetime > 1e-3f) ? proj.lifetime : 1.f;
        const float maxAlong = IntegratedDistanceAlongAim(proj, life);
        const float halfDist = maxAlong * 0.5f;
        if (foldedDistance > halfDist)
            foldedDistance = halfDist - (foldedDistance - halfDist);
    }

    outX = proj.startX + foldedDistance * cosf(proj.angle);
    outY = proj.startY + foldedDistance * sinf(proj.angle);

    if (proj.amplitude != 0.f) {
        const float life = (proj.lifetime > 0.001f) ? proj.lifetime : 1.f;
        const float phaseParity = ((proj.bulletId % 2) == 0) ? 0.f : kPI;
        const float lateral = proj.amplitude
            * sinf(phaseParity + ((tMs / life) * proj.frequency * 2.f * kPI));
        outX += lateral * cosf(proj.angle + kPI * 0.5f);
        outY += lateral * sinf(proj.angle + kPI * 0.5f);
    }
}

static void TryReadRuntimeChebyshevT(void* projInst, float& outT)
{
    outT = 0.f;
    if (!AddrOk(projInst)) return;
    const uint32_t off = RuntimeOffsets::Hbeak_ProjRadius;
    if (off == 0u || off >= 0x8000u) return;
    __try {
        float t = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(projInst) + off);
        if (t > 1e-4f && t < 16.f && std::isfinite(t)) outT = t;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// One-shot offset diagnostic. Dumps the first 12 enemy projectiles seen
// along with attackerObjId (so we can confirm shot diversity), named KJ
// sprite fields, every float between +0x040 and +0x200 on the HBEAK
// instance, and the first 0x120 bytes of projProps as floats. The real
// per-type size field will be the column whose values vary across
// different attackers.
static std::atomic<int> g_offsetLogCount{ 0 };

static void LogOffsetDiagnostic(void* projInst, int32_t bulletId,
                                int32_t attackerObjId, uint32_t ownerObjId,
                                float projHalfSize, float runtimeT,
                                float collMult, void* projPropsPtr,
                                bool hasCustomHitbox,
                                float customOffsetX, float customOffsetY,
                                bool isEnemyShot)
{
#ifndef _DEBUG
    // Investigation complete (see comment block at the per-type section
    // of this file). Compiled out in Release so we don't pay fopen/fprintf
    // on the spawn detour for every enemy shot during the first 12 fires
    // of every session, and so customer machines don't get %TEMP% spam.
    (void)projInst; (void)bulletId; (void)attackerObjId; (void)ownerObjId;
    (void)projHalfSize; (void)runtimeT; (void)collMult; (void)projPropsPtr;
    (void)hasCustomHitbox; (void)customOffsetX; (void)customOffsetY;
    (void)isEnemyShot;
    return;
#else
    if (!isEnemyShot) return;
    const int n = g_offsetLogCount.fetch_add(1, std::memory_order_relaxed);
    if (n >= 12) return;

    char path[MAX_PATH] = {};
    DWORD tmpLen = GetTempPathA(MAX_PATH, path);
    if (tmpLen == 0 || tmpLen >= MAX_PATH) return;
    strncat_s(path, sizeof(path), "LFG-offset-diag.log", _TRUNCATE);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || !f) return;

    if (n == 0) {
        fprintf(f,
            "=== LFG dodge-offset diagnostic (v2) ===\n"
            "Resolved: Hbeak_ProjRadius=0x%X  KJ_BaseRadius=0x%X  KJ_Scale=0x%X  KJ_SkinWidthObj=0x%X  PP_CollMult=0x%X\n"
            "Looking for: a field whose value differs between bullets fired by DIFFERENT attackerObjIds.\n"
            "A real per-type T is in the 0.05-0.7 range. The field we're currently using (0x1D4)\n"
            "always reads 0.5, which means it's a constant, not per-type T.\n\n",
            static_cast<unsigned>(RuntimeOffsets::Hbeak_ProjRadius),
            static_cast<unsigned>(RuntimeOffsets::KJ_BaseRadius),
            static_cast<unsigned>(RuntimeOffsets::KJ_Scale),
            static_cast<unsigned>(RuntimeOffsets::KJ_SkinWidthObj),
            static_cast<unsigned>(RuntimeOffsets::PP_CollMult));
    }

    // HBEAK instance dump: +0x040 .. +0x200  (448 bytes = 112 floats).
    constexpr int kHBeakStart = 0x040;
    constexpr int kHBeakCount = (0x200 - 0x040) / 4;
    float hb[kHBeakCount] = { 0 };
    float kjBaseR = 0.f, kjScale = 0.f, kjSkin = 0.f;
    if (AddrOk(projInst)) {
        __try {
            uint8_t* b = reinterpret_cast<uint8_t*>(projInst);
            for (int i = 0; i < kHBeakCount; ++i) {
                hb[i] = *reinterpret_cast<float*>(b + kHBeakStart + i * 4);
            }
            if (RuntimeOffsets::KJ_BaseRadius  && RuntimeOffsets::KJ_BaseRadius  < 0x8000)
                kjBaseR = *reinterpret_cast<float*>(b + RuntimeOffsets::KJ_BaseRadius);
            if (RuntimeOffsets::KJ_Scale       && RuntimeOffsets::KJ_Scale       < 0x8000)
                kjScale = *reinterpret_cast<float*>(b + RuntimeOffsets::KJ_Scale);
            if (RuntimeOffsets::KJ_SkinWidthObj && RuntimeOffsets::KJ_SkinWidthObj < 0x8000)
                kjSkin  = *reinterpret_cast<float*>(b + RuntimeOffsets::KJ_SkinWidthObj);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // projProps dump: +0x000 .. +0x120 (288 bytes = 72 floats).
    constexpr int kPpCount = 0x120 / 4;
    float pp[kPpCount] = { 0 };
    if (AddrOk(projPropsPtr)) {
        __try {
            uint8_t* ppb = reinterpret_cast<uint8_t*>(projPropsPtr);
            for (int i = 0; i < kPpCount; ++i) {
                pp[i] = *reinterpret_cast<float*>(ppb + i * 4);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    fprintf(f,
        "bullet[%2d] id=%d  attackerObjId=%d  ownerObjId=%u\n"
        "            projHalfSize=%.4f  runtimeT(0x%X)=%.4f  collMult=%.4f  hasCustomHitbox=%d  customOff=(%.4f,%.4f)\n"
        "            KJ_BaseRadius(0x%X)=%.4f  KJ_Scale(0x%X)=%.4f  KJ_SkinWidthObj(0x%X)=%.4f\n",
        n, bulletId, attackerObjId, ownerObjId,
        projHalfSize,
        static_cast<unsigned>(RuntimeOffsets::Hbeak_ProjRadius), runtimeT,
        collMult, hasCustomHitbox ? 1 : 0, customOffsetX, customOffsetY,
        static_cast<unsigned>(RuntimeOffsets::KJ_BaseRadius),  kjBaseR,
        static_cast<unsigned>(RuntimeOffsets::KJ_Scale),       kjScale,
        static_cast<unsigned>(RuntimeOffsets::KJ_SkinWidthObj), kjSkin);

    fprintf(f, "    HBEAK +0x%03X..+0x%03X (non-zero plausible-T entries flagged):\n",
        kHBeakStart, kHBeakStart + kHBeakCount * 4 - 4);
    for (int i = 0; i < kHBeakCount; ++i) {
        const uint32_t off = kHBeakStart + i * 4;
        const float v = hb[i];
        if (fabsf(v) < 1e-6f) continue;     // skip zeros — keeps log readable
        const char* flag = (std::isfinite(v) && v > 1e-4f && v < 2.f) ? " <-- plausible T" : "";
        fprintf(f, "        +0x%03X = %12.4f%s\n", off, v, flag);
    }
    fprintf(f, "    projProps +0x000..+0x%03X (non-zero flagged):\n", kPpCount * 4 - 4);
    for (int i = 0; i < kPpCount; ++i) {
        const uint32_t off = i * 4;
        const float v = pp[i];
        if (fabsf(v) < 1e-6f) continue;
        const char* flag = (std::isfinite(v) && v > 1e-4f && v < 2.f) ? " <-- plausible T" : "";
        fprintf(f, "        +0x%03X = %12.4f%s\n", off, v, flag);
    }
    fprintf(f, "\n");
    fclose(f);
#endif // _DEBUG
}


static bool TryReadLivePos(void* projInst, float& outX, float& outY)
{
    outX = 0.f;
    outY = 0.f;
    if (!AddrOk(projInst)) return false;
    __try {
        uint8_t* p = reinterpret_cast<uint8_t*>(projInst);
        outX = *reinterpret_cast<float*>(p + kOffWorldX);
        outY = *reinterpret_cast<float*>(p + kOffWorldY);
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

// Spawn often passes a shared ProjectileProperties template; the live per-shot copy is
// HBEAKBIHANL.FOMOIBCKIFP @ 0x118 (Il2CppInspector).
static void* EffectiveProjPropsFromHbeak(void* hbeak, void* argProjProps)
{
    if (AddrOk(hbeak)) {
        __try {
            void* p118 = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(hbeak) + RuntimeOffsets::Hbeak_ProjPropsPtr);
            if (AddrOk(p118)) return p118;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return argProjProps;
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
    (void)objProps;
    (void)name;
    (void)group;
    (void)canHitPlayer;
    (void)projInstance;

    {
        // Proves the spawn hook is installed AND the game is calling it. No
        // "detour FIRED" lines while bullets fly → wrong function hooked.
        // FIRED but XDodge projs=0 → failure is downstream (store/filter).
        static int s_n = 0;
        if ((s_n++ % 120) == 0)
            PtLogDetourFired(s_n, bulletId, attackerObjId, ownerObjId);
    }

    RuntimeOffsets::EnsureAll();

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

    const bool isEnemyShot = !isLocalShot;

    float entityX = 0.f, entityY = 0.f;
    LookupShooterOrigin(attackerObjId, ownerObjId, entityX, entityY);

    float sx, sy;
    if (fabsf(entityX) > 0.5f || fabsf(entityY) > 0.5f) {
        sx = entityX + spawnX;
        sy = entityY + spawnY;
    } else {
        float liveX = 0.f, liveY = 0.f;
        if (TryReadLivePos(ret, liveX, liveY) &&
            (fabsf(liveX) > 0.5f || fabsf(liveY) > 0.5f)) {
            sx = liveX;
            sy = liveY;
        } else {
            sx = spawnX;
            sy = spawnY;
        }
    }

    CRITICAL_SECTION* cs;
    WorldProjectile*  slots;
    uint32_t          maxSlots;
    std::atomic<uint32_t>* writeIdx;
    if (isEnemyShot) {
        cs = &g_RingCs; slots = g_Slots; maxSlots = kMaxTrackedProj; writeIdx = &g_WriteIdx;
    } else {
        if (!g_LocalCsInit) { InitializeCriticalSection(&g_LocalCs); g_LocalCsInit = true; }
        cs = &g_LocalCs; slots = g_LocalSlots; maxSlots = kMaxLocalProj; writeIdx = &g_LocalWriteIdx;
    }

    EnterCriticalSection(cs);
    const uint32_t idx = writeIdx->fetch_add(1, std::memory_order_relaxed) % maxSlots;
    WorldProjectile& p = slots[idx];
    memset(&p, 0, sizeof(p));
    p.startX = sx;
    p.startY = sy;
    if (!isEnemyShot) {
        g_MuzzleDbgSpawnX.store(sx, std::memory_order_relaxed);
        g_MuzzleDbgSpawnY.store(sy, std::memory_order_relaxed);
        g_MuzzleDbgHasSpawn.store(1, std::memory_order_relaxed);
    }
    p.angle = angle;
    p.spawnTick = spawnTickPre;
    p.valid = true;
    p.ptr = ret;
    p.bulletId = bulletId;
    p.attackerObjId = attackerObjId;
    p.ownerObjId = ownerObjId;
    p.speed = 5000.f;
    p.lifetime = 2000.f;
    p.minDamage = 100;
    p.damage = 100;
    p.isAccelerating = false;
    p.useAccel       = false;
    p.acceleration = 0.f;
    p.accelerationInv = 0.f;
    p.velocityChangeRate = 0.f;
    p.velocityChangeRateInv = 0.f;
    p.accelDelay = 0.f;
    p.speedClamp = 0.f;
    p.projPropsPtr = nullptr;

    void* const ppEffective = EffectiveProjPropsFromHbeak(ret, projProps);
    if (AddrOk(ppEffective)) {
        __try {
            p.projPropsPtr = ppEffective;
            uint8_t* pp = reinterpret_cast<uint8_t*>(ppEffective);
            float rawLifetime = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_Lifetime);
            p.lifetime      = NormalizeLifetimeToMs(rawLifetime);
            p.speed = static_cast<float>(*reinterpret_cast<int32_t*>(pp + RuntimeOffsets::PP_Speed));
            p.wavy = *reinterpret_cast<bool*>(pp + RuntimeOffsets::PP_IsWavy);
            p.hasCustomAmplitude = *reinterpret_cast<bool*>(pp + RuntimeOffsets::PP_HasCustomAmplitude);
            p.boomerang = *reinterpret_cast<bool*>(pp + RuntimeOffsets::PP_IsBoomerang);
            p.parametric = *reinterpret_cast<bool*>(pp + RuntimeOffsets::PP_IsParametric);
            p.frequency = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_Frequency);
            p.amplitude = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_Amplitude);
            p.minDamage = *reinterpret_cast<int32_t*>(pp + RuntimeOffsets::PP_MinDamage);
            p.damage = *reinterpret_cast<int32_t*>(pp + RuntimeOffsets::PP_MaxDamage);
            const float rawDelayF = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_AccelDelay);
            const float rawAccelF = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_Acceleration);
            p.isAccelerating = *reinterpret_cast<bool*>(pp + RuntimeOffsets::PP_IsAccel);
            p.useAccel       = *reinterpret_cast<bool*>(pp + RuntimeOffsets::PP_UseAccel);
            p.acceleration = rawAccelF;
            p.accelerationInv = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_AccelerationInv);
            p.velocityChangeRate = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_VelocityChangeRate);
            p.velocityChangeRateInv = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_VelocityChangeRateInv);
            p.accelDelay = NormalizeAccelDelayToMs(rawDelayF);
            p.speedClamp = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_SpeedClamp);

            // Per-type collision half-edge. Diagnostic log (%TEMP%\LFG-
            // offset-diag.log) confirmed on this build:
            //   * runtimeChebyshevHalf at HBEAK+0x1D4 is ALWAYS 0.5 — a
            //     static class constant, not per-bullet T.
            //   * collMult (PP+0xC0) is ALWAYS 1.0 for enemy shots.
            //   * KJ_BaseRadius × KJ_Scale at HBEAK+0x44 × +0x74 correctly
            //     differentiates per enemy type (e.g. 0.5×1.0=0.5 for a
            //     big-shot mob, 0.5×0.7=0.35 for a smaller one).
            // So sprite-derived product is the real source on this build;
            // the old collMult fallback treated every bullet as the same
            // huge size and over-stamped the planner.
            float collMult = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_CollMult);
            if (!std::isfinite(collMult) || collMult <= 0.f || collMult > 20.f) collMult = 1.0f;
            float projectileMagnitude = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_Magnitude);
            p.magnitude = projectileMagnitude;

            float kjBaseR = 0.f, kjScale = 0.f;
            if (AddrOk(ret)) {
                __try {
                    uint8_t* rb = reinterpret_cast<uint8_t*>(ret);
                    if (RuntimeOffsets::KJ_BaseRadius && RuntimeOffsets::KJ_BaseRadius < 0x8000)
                        kjBaseR = *reinterpret_cast<float*>(rb + RuntimeOffsets::KJ_BaseRadius);
                    if (RuntimeOffsets::KJ_Scale && RuntimeOffsets::KJ_Scale < 0x8000)
                        kjScale = *reinterpret_cast<float*>(rb + RuntimeOffsets::KJ_Scale);
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            if (std::isfinite(kjBaseR) && kjBaseR > 0.01f && kjBaseR < 4.f &&
                std::isfinite(kjScale) && kjScale > 0.01f && kjScale < 20.f) {
                p.projHalfSize = kjBaseR * kjScale;
            } else {
                p.projHalfSize = collMult * 0.5f;   // last-resort fallback
            }

            float laserDist = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_LaserDist);
            p.laserDistance = (laserDist > 1e-4f && std::isfinite(laserDist)) ? laserDist : 0.f;
            p.laser = p.laserDistance > 1e-3f;
            p.isTurning = *reinterpret_cast<bool*>(pp + RuntimeOffsets::PP_IsTurning);
            // PP[IsTurning+1]: "circle-turn delayed" flag (LDONHCKAFIA assembly, PP[0x1B1]).
            p.isCircleTurnDelayed = *reinterpret_cast<bool*>(pp + RuntimeOffsets::PP_IsTurning + 1);
            p.isTurningDelayed = *reinterpret_cast<bool*>(pp + RuntimeOffsets::PP_IsTurningDelayed);
            // PP[IsTurning+5]: snap-to-straight flag (LDONHCKAFIA assembly, PP[0x1B5]).
            // When 0 (common), arc continues past TurnStopTime for the full lifetime.
            p.turnSnapsToStraight = *reinterpret_cast<bool*>(pp + RuntimeOffsets::PP_IsTurning + 5);
            p.isTurningAccelerated = *reinterpret_cast<bool*>(pp + RuntimeOffsets::PP_IsTurning + 3);
            p.turnRate = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_TurnRate);
            if (!std::isfinite(p.turnRate)) p.turnRate = 0.f;
            {
                const float rawTurnStopTime = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_TurnStopTime);
                p.turnStopTime = (std::isfinite(rawTurnStopTime) && rawTurnStopTime > 0.f) ? rawTurnStopTime : 0.f;
                const float rawTurnRateDelay = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_TurnRateDelay);
                p.turnRateDelay = NormalizeAccelDelayToMs(rawTurnRateDelay);
                const float rawCircleAngle = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_CircleTurnAngle);
                p.circleTurnAngle = std::isfinite(rawCircleAngle) ? rawCircleAngle : 0.f;
                const float rawCircleDelay = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_CircleTurnDelay);
                p.circleTurnDelay = (std::isfinite(rawCircleDelay) && rawCircleDelay > 0.f) ? rawCircleDelay : 0.f;
                const float rawTurnAccel = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_TurnAcceleration);
                p.turnAcceleration = std::isfinite(rawTurnAccel) ? rawTurnAccel : 0.f;
                const float rawTurnAccelDelay = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_TurnAccelDelay);
                p.turnAccelDelay = std::isfinite(rawTurnAccelDelay) ? rawTurnAccelDelay : 0.f;
                const float rawTurnClamp = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_TurnClamp);
                p.turnClamp = std::isfinite(rawTurnClamp) ? rawTurnClamp : 0.f;
                const float rawTurnAccelInv = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_TurnAccelInv);
                p.turnAccelInv = std::isfinite(rawTurnAccelInv) ? rawTurnAccelInv : 0.f;
            }
            bool hasCustomHitbox = *reinterpret_cast<bool*>(pp + RuntimeOffsets::PP_HasCustomHitbox);
            p.hasCustomHitbox = hasCustomHitbox;

            if (hasCustomHitbox) {
                void* customHitbox = *reinterpret_cast<void**>(pp + RuntimeOffsets::PP_CustomHitbox);
                if (AddrOk(customHitbox)) {
                    uint8_t* ch = reinterpret_cast<uint8_t*>(customHitbox);
                    float offX = *reinterpret_cast<float*>(ch + RuntimeOffsets::CH_OffsetX);
                    float offY = *reinterpret_cast<float*>(ch + RuntimeOffsets::CH_OffsetY);
                    p.customOffsetX = offX;
                    p.customOffsetY = offY;
                    float hx = fabsf(offX), hy = fabsf(offY);
                    p.projHalfSize = (hx > hy) ? hx : hy;
                }
            }

            TryReadRuntimeChebyshevT(ret, p.runtimeChebyshevHalf);
            LogOffsetDiagnostic(ret, p.bulletId, p.attackerObjId, ownerObjId,
                                p.projHalfSize, p.runtimeChebyshevHalf,
                                collMult, pp,
                                p.hasCustomHitbox, p.customOffsetX, p.customOffsetY,
                                isEnemyShot);

            if (p.speed < 1.f || p.speed > 50000.f || p.lifetime < 50.f || p.lifetime > 600000.f) {
                p.speed = 5000.f;
                p.lifetime = 2000.f;
                p.minDamage = 100;
                p.damage = 100;
            }

        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    p.speedMul = ComputeEffectiveSpeedMulFromInstance(ret);

    if (AddrOk(ret)) {
        __try {
            uint8_t* pi = reinterpret_cast<uint8_t*>(ret);
            p.x = *reinterpret_cast<float*>(pi + kOffWorldX);
            p.y = *reinterpret_cast<float*>(pi + kOffWorldY);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ProjPosAt(p, 0.f, p.x, p.y);
        }
    } else {
        ProjPosAt(p, 0.f, p.x, p.y);
    }

    // Snapshot the slot (still under lock) for the hazard-spawn callback so the
    // DangerPlanner can trigger a replan on new enemy shots inside its horizon.
    const bool    callCallback = isEnemyShot;
    WorldProjectile snap;
    if (callCallback) {
        snap = p;
    }

    LeaveCriticalSection(cs);

    {
        // enemy=1 entries land in g_Slots[] — exactly what XDodge's
        // CopyActiveForDraw reads. enemy=1 here but XDodge projs=0 → the
        // CopyActiveForDraw lifetime/elapsed filter is dropping them.
        static int s_n = 0;
        if ((s_n++ % 120) == 0)
            PtLogStored(idx, (int)isEnemyShot, sx, sy, s_n);
    }

    if (callCallback) {
        // Seed per-dungeon type catalog (debug viz only — planner doesn't read).
        // Owner type passed as 0 here; resolution from WorldTAB is done later by
        // debug tools that care. Same-bullet/0-owner entries deduplicate safely.
        ProjectileCatalog::RecordSpawn(0, snap);

        ProjectileTracking::HazardSpawnCb cb = nullptr;
        void* user = nullptr;
        {
            EnterCriticalSection(&g_RingCs);
            cb   = g_HazardCb;
            user = g_HazardCbUser;
            LeaveCriticalSection(&g_RingCs);
        }
        if (cb) {
            __try {
                cb(snap, user);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    return ret;
}

static void RefreshMotionFromProjProps(WorldProjectile& dst, void* projPropsPtr)
{
    if (!AddrOk(projPropsPtr)) return;
    __try {
        uint8_t* pp = reinterpret_cast<uint8_t*>(projPropsPtr);
        dst.acceleration = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_Acceleration);
        dst.accelerationInv = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_AccelerationInv);
        dst.velocityChangeRate = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_VelocityChangeRate);
        dst.velocityChangeRateInv = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_VelocityChangeRateInv);
        dst.accelDelay = NormalizeAccelDelayToMs(*reinterpret_cast<float*>(pp + RuntimeOffsets::PP_AccelDelay));
        dst.speedClamp = *reinterpret_cast<float*>(pp + RuntimeOffsets::PP_SpeedClamp);
        dst.isAccelerating = *reinterpret_cast<bool*>(pp + RuntimeOffsets::PP_IsAccel);
        dst.useAccel       = *reinterpret_cast<bool*>(pp + RuntimeOffsets::PP_UseAccel);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void FillOutFromSlot(WorldProjectile& dst, const WorldProjectile& src, ULONGLONG now, bool livePos)
{
    dst = src;
    if (!src.valid) return;
    RefreshMotionFromProjProps(dst, src.projPropsPtr);
    if (AddrOk(src.ptr)) {
        float rt = 0.f;
        TryReadRuntimeChebyshevT(src.ptr, rt);
        dst.runtimeChebyshevHalf = (rt > 1e-5f) ? rt : src.runtimeChebyshevHalf;
    }
    float elapsed = static_cast<float>(now - src.spawnTick);
    if (livePos && AddrOk(src.ptr) && TryReadLivePos(src.ptr, dst.x, dst.y)) {
        // Re-anchor startX/startY so ComputePosAt stays consistent with the live
        // position. Without this, stale entity positions at spawn time cause
        // predicted positions to drift far from reality.
        //
        // Exception: parametric shots use startX/Y as the orbit CENTER — shifting
        // it by the instantaneous orbit displacement would move the entire circle
        // each frame, making future predictions wrong.
        // Same applies to ALL turning variants (isTurning / isCircleTurnDelayed /
        // isTurningDelayed / isTurningAccelerated) — the spiral/arc math at the
        // top of ComputePosAt uses startX/Y as the rotation center, so per-frame
        // re-anchoring walks the spiral around the live position and every
        // future-tick prediction lands in the wrong place.
        // Lasers are stationary (BJLDGDKMPFL::positionAt always returns spawn origin),
        // and dodge code never uses ComputePosAt for them — DangerMap stamps the beam
        // as a capsule and PrecisionDodge does its own line-distance check. Skipping
        // the re-anchor keeps startX as the beam's true emitter origin.
        const bool reAnchorSafe = !src.parametric && !src.laser
            && !src.isTurning && !src.isCircleTurnDelayed
            && !src.isTurningDelayed && !src.isTurningAccelerated;
        if (reAnchorSafe && elapsed >= 0.f) {
            float predX, predY;
            ProjPosAt(dst, elapsed, predX, predY);
            dst.startX += (dst.x - predX);
            dst.startY += (dst.y - predY);
        }
        return;
    }
    if (elapsed >= 0.f)
        ProjPosAt(dst, elapsed + kProjVisualTimeOffsetMs, dst.x, dst.y);
    else {
        dst.x = src.x;
        dst.y = src.y;
    }
}

} // namespace

namespace ProjectileTracking {

static void* g_spawnTarget = nullptr;

void Install()
{
    if (g_Installed) return;
    {
        static int s_n = 0;
        if ((s_n++ % 240) == 0)
            DBG_FILE_LOG("[ProjectileTracking] Install() reached, not yet installed "
                "(attempt=" << s_n << ") — resolving hook target...");
    }
    if (!g_CsInit) {
        InitializeCriticalSection(&g_RingCs);
        InitializeCriticalSection(&g_EntCs);
        g_CsInit = true;
    }

    Il2CppClass* klass = ResolveProjClass();
    if (!klass) {
        static int s_n = 0;
        if ((s_n++ % 240) == 0)
            DBG_FILE_LOG("[ProjectileTracking] Install: projectile class '"
                << kProjClassName << "' UNRESOLVED — BeeByte name stale for this "
                "game build. No bullets captured → XDodge has nothing to dodge. "
                "(attempt=" << s_n << ")");
        return;
    }
    const MethodInfo* mi = il2cpp_class_get_method_from_name(klass, kSpawnMethodName, kSpawnParamCount);
    if (!mi || !mi->methodPointer) {
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
    if (MH_EnableHook(g_spawnTarget) != MH_OK)
        return;

    EnsureHbeakSpeedMulFieldOffset();
    g_Installed = true;
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
    if (g_CsInit) {
        DeleteCriticalSection(&g_RingCs);
        DeleteCriticalSection(&g_EntCs);
        g_CsInit = false;
    }
}

void ComputePosAtSafe(const WorldProjectile& proj, float tMs, float& outX, float& outY)
{
    outX = 0.f;
    outY = 0.f;
    __try { ComputePosAt(proj, tMs, outX, outY); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SetLocalPlayerObjectId(int32_t objectId)
{
    g_LocalDictKey.store(objectId, std::memory_order_relaxed);
}

int32_t GetLocalPlayerObjectId()
{
    return g_LocalDictKey.load(std::memory_order_relaxed);
}

bool GetMuzzleDebugLastSpawn(float& outWorldX, float& outWorldY)
{
    if (!g_MuzzleDbgHasSpawn.load(std::memory_order_relaxed))
        return false;
    outWorldX = g_MuzzleDbgSpawnX.load(std::memory_order_relaxed);
    outWorldY = g_MuzzleDbgSpawnY.load(std::memory_order_relaxed);
    return std::isfinite(outWorldX) && std::isfinite(outWorldY);
}

void OnWorldRefreshBegin()
{
    EnterCriticalSection(&g_EntCs);
    g_EntityPos.clear();
    LeaveCriticalSection(&g_EntCs);
}

void OnWorldEntity(int32_t objectId, float x, float y)
{
    EnterCriticalSection(&g_EntCs);
    g_EntityPos[objectId] = { x, y };
    LeaveCriticalSection(&g_EntCs);
}

void SetVisualFlags(bool showPaths, bool showHitboxes)
{
    g_ShowPaths.store(showPaths, std::memory_order_relaxed);
    g_ShowHitboxes.store(showHitboxes, std::memory_order_relaxed);
}

bool ShowPaths() { return g_ShowPaths.load(std::memory_order_relaxed); }
bool ShowHitboxes() { return g_ShowHitboxes.load(std::memory_order_relaxed); }

void SnapshotToWorld(std::vector<WorldProjectile>& out)
{
    out.clear();
    const ULONGLONG now = GetTickCount64();
    EnterCriticalSection(&g_RingCs);
    for (int i = 0; i < kMaxTrackedProj; ++i) {
        const WorldProjectile& s = g_Slots[i];
        if (!s.valid) continue;
        float elapsed = static_cast<float>(now - s.spawnTick);
        if (s.lifetime > 0.f && elapsed >= s.lifetime) continue;

        WorldProjectile row;
        FillOutFromSlot(row, s, now, true);
        out.push_back(row);
    }
    LeaveCriticalSection(&g_RingCs);
}

void CopyActiveForDraw(std::vector<WorldProjectile>& out)
{
    out.clear();
    const ULONGLONG now = GetTickCount64();
    int dbgValid = 0, dbgExpired = 0, dbgFuture = 0;
    EnterCriticalSection(&g_RingCs);
    for (int i = 0; i < kMaxTrackedProj; ++i) {
        const WorldProjectile& s = g_Slots[i];
        if (!s.valid) continue;
        ++dbgValid;
        float elapsed = static_cast<float>(now - s.spawnTick);
        float elapsedViz = elapsed + kProjVisualTimeOffsetMs;
        if (elapsedViz < 0.f) { ++dbgFuture; continue; }
        if (s.lifetime > 0.f && elapsedViz >= s.lifetime) { ++dbgExpired; continue; }

        WorldProjectile row;
        FillOutFromSlot(row, s, now, true);
        out.push_back(row);
    }
    LeaveCriticalSection(&g_RingCs);

    {
        // Disambiguates XDodge's projs=0:
        //   validSlots=0                 → nothing ever captured (detour/hook)
        //   validSlots>0 returned=0      → captured but filtered: expired=N
        //     (lifetime/spawnTick wrong) or future=N (clock/spawnTick skew)
        //   returned>0 but XDodge projs=0→ mismatch between this & XDodge call
        static int s_n = 0;
        if ((s_n++ % 120) == 0)
            DBG_FILE_LOG("[ProjectileTracking] CopyActiveForDraw: validSlots=" << dbgValid
                << " returned=" << out.size()
                << " expiredFiltered=" << dbgExpired
                << " futureFiltered=" << dbgFuture
                << " (call=" << s_n << ")");
    }
}

int CountValidForDiagnostics()
{
    const ULONGLONG now = GetTickCount64();
    int n = 0;
    EnterCriticalSection(&g_RingCs);
    for (int i = 0; i < kMaxTrackedProj; ++i) {
        const WorldProjectile& s = g_Slots[i];
        if (!s.valid) continue;
        float elapsed = static_cast<float>(now - s.spawnTick);
        if (s.lifetime > 0.f && elapsed >= s.lifetime) continue;
        ++n;
    }
    LeaveCriticalSection(&g_RingCs);
    return n;
}

void CopyActiveLocalForDraw(std::vector<WorldProjectile>& out)
{
    out.clear();
    if (!g_LocalCsInit) return;
    const ULONGLONG now = GetTickCount64();
    EnterCriticalSection(&g_LocalCs);
    for (int i = 0; i < kMaxLocalProj; ++i) {
        const WorldProjectile& s = g_LocalSlots[i];
        if (!s.valid) continue;
        float elapsed = static_cast<float>(now - s.spawnTick);
        if (s.lifetime <= 0.f) continue;
        if (elapsed >= s.lifetime) continue;
        WorldProjectile row;
        FillOutFromSlot(row, s, now, true);
        out.push_back(row);
    }
    LeaveCriticalSection(&g_LocalCs);
}

void ComputePosAt(const WorldProjectile& proj, float tMs, float& outX, float& outY)
{
    ProjPosAt(proj, tMs, outX, outY);
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
    return NormalizeLifetimeToMs(rawFromProps);
}

float NormalizeAccelDelayMs(float rawFromProps)
{
    return NormalizeAccelDelayToMs(rawFromProps);
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
    if (!g_CsInit) {
        // Critical sections are initialised inside Install(). If the caller
        // (e.g. DangerPlanner::TryInstall) wins the race before ProjectileTracking
        // self-installs, do a direct assignment — the spawn detour hasn't been
        // attached yet, so nothing else can be touching these globals.
        g_HazardCb     = cb;
        g_HazardCbUser = user;
        return;
    }
    EnterCriticalSection(&g_RingCs);
    g_HazardCb     = cb;
    g_HazardCbUser = user;
    LeaveCriticalSection(&g_RingCs);
}

void ClearHazardSpawnCallback()
{
    if (!g_CsInit) {
        g_HazardCb     = nullptr;
        g_HazardCbUser = nullptr;
        return;
    }
    EnterCriticalSection(&g_RingCs);
    g_HazardCb     = nullptr;
    g_HazardCbUser = nullptr;
    LeaveCriticalSection(&g_RingCs);
}

} // namespace ProjectileTracking
