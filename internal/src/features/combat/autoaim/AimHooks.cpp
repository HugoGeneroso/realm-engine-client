#include "pch-il2cpp.h"

#include "AimHooks.h"
#include "WeaponProfile.h"
#include "GameState.h"
#include "RuntimeOffsets.h"
#include "Il2CppResolver.h"
#include "minhook/MinHook.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace {

// ── IL2CPP method names ───────────────────────────────────────────────────────
static const char* kPlayerClass   = "LKHPPBEGNOM";
static const char* kShootClass    = "FKALGHJIADI";
static const char* kCSAMethod     = "ELCBJAFBLJG"; // ComputeShootAngle
static const char* kSWAMethod     = "EHGHCACPAGH"; // ShootWithAngle
static const char* kSSPMethod     = "PMIANFBMMNN"; // SendShotPacket

static const uint32_t& kOffPosX       = RuntimeOffsets::PosX;
static const uint32_t& kOffPosY       = RuntimeOffsets::PosY;
// shotData+0x1C is the angle field in the SHOOT packet struct
static constexpr uint32_t kOffShotAngle = 0x1C;

// ── Weapon-specific proj IDs ──────────────────────────────────────────────────
static constexpr int32_t kProjIdCultStaff    = 0xB0EB; // Staff of Unholy Sacrifice
static constexpr int32_t kProjIdColossusSlash = 0xB106; // Sword of the Colossus

// ── Shared aim state (written each tick by AutoAim coordinator) ───────────────
static std::atomic<bool>  s_hasTarget{ false };
static std::atomic<float> s_targetX{ 0.f };
static std::atomic<float> s_targetY{ 0.f };
static std::atomic<bool>  s_reverseCultStaff{ true };
static std::atomic<bool>  s_offsetColossus{ false };
static std::atomic<bool>  s_enabled{ false };

// ── Hook function-pointer types ───────────────────────────────────────────────
using ComputeShootAngleFn = void(__fastcall*)(void*, uint8_t, float*, bool*, bool, void*);
using ShootWithAngleFn    = void(__fastcall*)(void*, float, void*);
using SendShotPacketFn    = void(__fastcall*)(void*, void*, int32_t, void*);

static ComputeShootAngleFn g_csaOrig = nullptr;
static ShootWithAngleFn    g_swaOrig = nullptr;
static SendShotPacketFn    g_sspOrig = nullptr;
static void* g_csaTarget = nullptr;
static void* g_swaTarget = nullptr;
static void* g_sspTarget = nullptr;
static bool  s_installed = false;

static inline bool AddrOk(const void* p) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    return a > 0x10000 && a < 0x7FFFFFFFFFFFULL;
}

static float ApplyWeaponTweaks(float angle)
{
    const int32_t pid = WeaponCalibrator::GetProfile().projId;
    if (s_reverseCultStaff.load(std::memory_order_relaxed) && pid == kProjIdCultStaff)
        angle += 3.14159265f;
    if (s_offsetColossus.load(std::memory_order_relaxed) && pid == kProjIdColossusSlash)
        angle += 0.f; // TODO: extract exact offset from Multitool DLL
    return angle;
}

static bool ShouldRedirect(void* player)
{
    if (!s_enabled.load(std::memory_order_relaxed)) return false;
    if (!s_hasTarget.load(std::memory_order_relaxed)) return false;
    if (!AddrOk(player)) return false;
    void* local = GameState::GetLocalPtr();
    return local && player == local;
}

// ── Detour implementations ────────────────────────────────────────────────────
void __fastcall ComputeShootAngleDetour(
    void* player, uint8_t slot, float* outAngle, bool* outCanShoot, bool boolArg, void* method)
{
    g_csaOrig(player, slot, outAngle, outCanShoot, boolArg, method);
    if (!ShouldRedirect(player) || !outAngle) return;

    float px = 0.f, py = 0.f;
    __try {
        uint8_t* lp = reinterpret_cast<uint8_t*>(player);
        px = *reinterpret_cast<float*>(lp + kOffPosX);
        py = *reinterpret_cast<float*>(lp + kOffPosY);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    *outAngle = ApplyWeaponTweaks(atan2f(
        s_targetY.load(std::memory_order_relaxed) - py,
        s_targetX.load(std::memory_order_relaxed) - px));
}

void __fastcall ShootWithAngleDetour(void* player, float angle, void* method)
{
    if (ShouldRedirect(player)) {
        float px = 0.f, py = 0.f;
        bool ok = false;
        __try {
            uint8_t* lp = reinterpret_cast<uint8_t*>(player);
            px = *reinterpret_cast<float*>(lp + kOffPosX);
            py = *reinterpret_cast<float*>(lp + kOffPosY);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (ok) {
            angle = ApplyWeaponTweaks(atan2f(
                s_targetY.load(std::memory_order_relaxed) - py,
                s_targetX.load(std::memory_order_relaxed) - px));
        }
    }
    g_swaOrig(player, angle, method);
}

void __fastcall SendShotPacketDetour(void* player, void* shotData, int32_t projCount, void* method)
{
    if (ShouldRedirect(player) && AddrOk(shotData) &&
        AddrOk(reinterpret_cast<const uint8_t*>(shotData) + 0x24) && projCount > 0)
    {
        float px = 0.f, py = 0.f;
        bool ok = false;
        __try {
            uint8_t* lp = reinterpret_cast<uint8_t*>(player);
            px = *reinterpret_cast<float*>(lp + kOffPosX);
            py = *reinterpret_cast<float*>(lp + kOffPosY);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (ok) {
            const float newAngle = ApplyWeaponTweaks(atan2f(
                s_targetY.load(std::memory_order_relaxed) - py,
                s_targetX.load(std::memory_order_relaxed) - px));
            __try {
                *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(shotData) + kOffShotAngle) = newAngle;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            __try {
                *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(player) +
                    RuntimeOffsets::Player_FacingAngle) = newAngle;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    g_sspOrig(player, shotData, projCount, method);
}

static void* ResolveMethod(const char* cls, const char* method, int params)
{
    Il2CppClass* klass = Resolver::GetClass("", cls);
    if (!klass) return nullptr;
    const MethodInfo* mi = il2cpp_class_get_method_from_name(klass, method, params);
    return (mi && mi->methodPointer) ? reinterpret_cast<void*>(mi->methodPointer) : nullptr;
}

} // namespace

namespace AimHooks {

bool Install()
{
    if (s_installed) return true;

    g_csaTarget = ResolveMethod(kPlayerClass, kCSAMethod, 4);
    g_swaTarget = ResolveMethod(kShootClass,  kSWAMethod, 1);
    g_sspTarget = ResolveMethod(kShootClass,  kSSPMethod, 2);
    if (!g_csaTarget || !g_swaTarget || !g_sspTarget) return false;

    static bool s_mhInit = false;
    if (!s_mhInit) {
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return false;
        s_mhInit = true;
    }

    if (MH_CreateHook(g_csaTarget, reinterpret_cast<void*>(&ComputeShootAngleDetour),
                      reinterpret_cast<void**>(&g_csaOrig)) != MH_OK) return false;
    if (MH_CreateHook(g_swaTarget, reinterpret_cast<void*>(&ShootWithAngleDetour),
                      reinterpret_cast<void**>(&g_swaOrig)) != MH_OK) return false;
    if (MH_CreateHook(g_sspTarget, reinterpret_cast<void*>(&SendShotPacketDetour),
                      reinterpret_cast<void**>(&g_sspOrig)) != MH_OK) return false;

    MH_EnableHook(g_csaTarget);
    MH_EnableHook(g_swaTarget);
    MH_EnableHook(g_sspTarget);

    s_installed = true;
    return true;
}

void Uninstall()
{
    if (!s_installed) return;
    s_enabled.store(false, std::memory_order_release);
    if (g_csaTarget) { MH_DisableHook(g_csaTarget); MH_RemoveHook(g_csaTarget); }
    if (g_swaTarget) { MH_DisableHook(g_swaTarget); MH_RemoveHook(g_swaTarget); }
    if (g_sspTarget) { MH_DisableHook(g_sspTarget); MH_RemoveHook(g_sspTarget); }
    g_csaOrig = nullptr; g_swaOrig = nullptr; g_sspOrig = nullptr;
    g_csaTarget = g_swaTarget = g_sspTarget = nullptr;
    s_installed = false;
}

bool IsInstalled() { return s_installed; }

void SetTarget(bool hasTarget, float x, float y)
{
    s_hasTarget.store(hasTarget, std::memory_order_relaxed);
    s_targetX.store(x, std::memory_order_relaxed);
    s_targetY.store(y, std::memory_order_relaxed);
    s_enabled.store(true, std::memory_order_relaxed);
}

void SetReverseCultStaff(bool v)   { s_reverseCultStaff.store(v, std::memory_order_relaxed); }
void SetOffsetColossusSword(bool v) { s_offsetColossus.store(v, std::memory_order_relaxed); }

} // namespace AimHooks
