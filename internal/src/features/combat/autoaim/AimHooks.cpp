#include "pch-il2cpp.h"

#include "AimHooks.h"
#include "WeaponProfile.h"
#include "GameState.h"
#include "RuntimeOffsets.h"
#include "Il2CppResolver.h"
#include "DbgFileLog.h"
#include "LocalPlayer.h"
#include "minhook/MinHook.h"
#include "FeatureState.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace {

// ── IL2CPP method names ───────────────────────────────────────────────────────
static const char* kPlayerClass   = "LKHPPBEGNOM";
static const char* kShootClass    = "FKALGHJIADI";
static const char* kPlayerFireCls = "KHIPANODLID"; // PlayerFire — actual ShootWithAngle on Exalt
static const char* kCSAMethod     = "ELCBJAFBLJG"; // ComputeShootAngle
static const char* kSWAMethod     = "EHGHCACPAGH"; // ShootWithAngle
static const char* kSSPMethod     = "PMIANFBMMNN"; // SendShotPacket

static const uint32_t& kOffPosX       = RuntimeOffsets::PosX;
static const uint32_t& kOffPosY       = RuntimeOffsets::PosY;

// SendShotPacket hook disabled — hardcoded shotData+0x1C corrupts the packet struct
// on current Exalt builds and crashes on first redirected shot.
static constexpr bool kEnableSendShotPacketHook = false;

// ── Weapon-specific proj IDs ──────────────────────────────────────────────────
static constexpr int32_t kProjIdCultStaff    = 0xB0EB;
static constexpr int32_t kProjIdColossusSlash = 0xB106;

static std::atomic<bool>  s_aimActive{ false };
static std::atomic<bool>  s_hasTarget{ false };
static std::atomic<float> s_targetX{ 0.f };
static std::atomic<float> s_targetY{ 0.f };
static std::atomic<bool>  s_reverseCultStaff{ true };
static std::atomic<bool>  s_offsetColossus{ false };

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

static std::atomic<uint64_t> s_csaCalls{ 0 };
static std::atomic<uint64_t> s_csaRedirect{ 0 };
static std::atomic<uint64_t> s_swaCalls{ 0 };
static std::atomic<uint64_t> s_swaRedirect{ 0 };

static std::atomic<uint64_t> s_autoFireShots{ 0 };
static std::atomic<void*>    s_lastSwaThis{ nullptr };
static ULONGLONG               s_lastAutoFireMs = 0;
static constexpr ULONGLONG     kAutoFireCooldownMs = 160ULL;

using SetAutoFireFn = void(__fastcall*)(void*, bool, void*);
static SetAutoFireFn           g_setAutoFireFn = nullptr;
static std::atomic<bool>       s_nativeAutoFireOn{ false };
static std::atomic<bool>       s_setAutoFireResolved{ false };

static constexpr int kNumCandidates = 5;
static const char* kCandidateNames[kNumCandidates] = {
    "ODLMLAOBIIH", // old set_AutoFire
    "HAGABBBCKID", // new candidate 1
    "JFPOCDGGOBM", // new candidate 2
    "OAFCNKLLIBC", // new candidate 3
    "GOKDJOEJANB"  // new candidate 4
};
static SetAutoFireFn g_autoFireFns[kNumCandidates] = { nullptr };
static std::atomic<int> s_activeAutoFireCandidateIdx{ -1 };

static bool IsGameForeground()
{
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

static bool TryInvokeShoot(void* local, float angle)
{
    if (!g_swaOrig || !local || !std::isfinite(angle)) return false;
    __try {
        g_swaOrig(local, angle, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

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
        angle += 0.f;
    return angle;
}

static bool ShouldRedirect(void* player)
{
    if (!s_aimActive.load(std::memory_order_relaxed)) return false;
    if (!s_hasTarget.load(std::memory_order_relaxed)) return false;
    if (!AddrOk(player)) return false;
    void* local = GameState::GetLocalPtr();
    if (!local) return false;
    if (player == local) return true;
    // CSA may run on LKHPPBEGNOM while GetLocalPtr() is FKALGHJIADI (subclass).
    __try {
        auto* pk = reinterpret_cast<Il2CppClass*>(*reinterpret_cast<void**>(player));
        auto* lk = reinterpret_cast<Il2CppClass*>(*reinterpret_cast<void**>(local));
        if (pk == lk) return true;
        if (pk && lk) {
            if (il2cpp_class_is_assignable_from(pk, lk)) return true;
            if (il2cpp_class_is_assignable_from(lk, pk)) return true;
        }
        // SWA runs on FKALGHJIADI (shoot component); local is LKHPPBEGNOM (player root).
        Il2CppClass* shootKlass = Resolver::GetClass("", kShootClass);
        if (!shootKlass) shootKlass = Resolver::FindClassLoose(kShootClass);
        if (pk && shootKlass) {
            if (pk == shootKlass) return true;
            if (il2cpp_class_is_assignable_from(shootKlass, pk)) return true;
            if (il2cpp_class_is_assignable_from(pk, shootKlass)) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// SWA is always the local player's shoot path — do not require pointer match with GetLocalPtr().
static bool ShouldRedirectSwA()
{
    if (!s_aimActive.load(std::memory_order_acquire)) return false;
    if (!s_hasTarget.load(std::memory_order_acquire)) return false;
    return GameState::GetLocalPtr() != nullptr;
}

static bool ReadAimPlayerPos(void* /*player*/, float& px, float& py)
{
    float wx = 0.f, wy = 0.f;
    if (FeatureState::TryGetClientPos(wx, wy) && (fabsf(wx) > 1.f || fabsf(wy) > 1.f)) {
        px = wx;
        py = wy;
        return true;
    }

    const float lx = LocalPlayer::GetX();
    const float ly = LocalPlayer::GetY();
    if (std::isfinite(lx) && std::isfinite(ly) && (fabsf(lx) > 1.f || fabsf(ly) > 1.f)) {
        px = lx;
        py = ly;
        return true;
    }

    void* player = GameState::GetLocalPtr();
    if (!AddrOk(player))
        return false;

    __try {
        uint8_t* lp = reinterpret_cast<uint8_t*>(player);
        px = *reinterpret_cast<float*>(lp + kOffPosX);
        py = *reinterpret_cast<float*>(lp + kOffPosY);
        return std::isfinite(px) && std::isfinite(py);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ComputeRedirectAngle(void* player, float& outAngle)
{
    const float tx = s_targetX.load(std::memory_order_acquire);
    const float ty = s_targetY.load(std::memory_order_acquire);
    if (!std::isfinite(tx) || !std::isfinite(ty)) return false;

    float px = 0.f, py = 0.f;
    if (!ReadAimPlayerPos(player, px, py)) return false;
    if (!std::isfinite(px) || !std::isfinite(py)) return false;

    outAngle = ApplyWeaponTweaks(atan2f(ty - py, tx - px));
    return std::isfinite(outAngle) && outAngle >= -4.f * 3.14159265f && outAngle <= 4.f * 3.14159265f;
}

// ── Detour implementations ────────────────────────────────────────────────────
void __fastcall ComputeShootAngleDetour(
    void* player, uint8_t slot, float* outAngle, bool* outCanShoot, bool boolArg, void* method)
{
    s_csaCalls.fetch_add(1, std::memory_order_relaxed);
    __try {
        if (g_csaOrig)
            g_csaOrig(player, slot, outAngle, outCanShoot, boolArg, method);
        if (!ShouldRedirect(player) || !outAngle) return;

        float angle = 0.f;
        if (ComputeRedirectAngle(player, angle)) {
            *outAngle = angle;
            s_csaRedirect.fetch_add(1, std::memory_order_relaxed);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void __fastcall ShootWithAngleDetour(void* player, float angle, void* method)
{
    s_swaCalls.fetch_add(1, std::memory_order_relaxed);
    if (AddrOk(player))
        s_lastSwaThis.store(player, std::memory_order_relaxed);

    float useAngle = angle;
    __try {
        if (ShouldRedirectSwA()) {
            float redirected = angle;
            if (ComputeRedirectAngle(player, redirected)) {
                useAngle = redirected;
                s_swaRedirect.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        useAngle = angle;
    }

    if (g_swaOrig) {
        __try {
            g_swaOrig(player, useAngle, method);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

void __fastcall SendShotPacketDetour(void* player, void* shotData, int32_t projCount, void* method)
{
    __try {
        if (g_sspOrig)
            g_sspOrig(player, shotData, projCount, method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void* ResolveMethod(const char* cls, const char* method, int params)
{
    Il2CppClass* klass = Resolver::GetClass("", cls);
    if (!klass) klass = Resolver::FindClassLoose(cls);
    if (!klass) return nullptr;
    const MethodInfo* mi = il2cpp_class_get_method_from_name(klass, method, params);
    if (mi && mi->methodPointer && AddrOk(mi->methodPointer))
        return reinterpret_cast<void*>(mi->methodPointer);
    // Method name stale after BeeByte pass — fall back to arity match.
    void* iter = nullptr;
    for (const MethodInfo* cand; (cand = il2cpp_class_get_methods(klass, &iter)) != nullptr; ) {
        if (cand->parameters_count == params && cand->methodPointer && AddrOk(cand->methodPointer))
            return reinterpret_cast<void*>(cand->methodPointer);
    }
    return nullptr;
}

__declspec(noinline) static void* SafeResolveMethod(const char* cls, const char* method, int params)
{
    void* target = nullptr;
    __try {
        target = ResolveMethod(cls, method, params);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        target = nullptr;
    }
    return target;
}

static void ResolveSetAutoFireOnce()
{
    if (s_setAutoFireResolved.exchange(true, std::memory_order_relaxed))
        return;
    static const char* kClasses[] = {
        "AAGOIIEJOMO", // PlayerFire (new)
        "KHIPANODLID", // PlayerFire (old)
        "DLENNMIMCDN", // PlayerInputBot
        "FKALGHJIADI",
        "LKHPPBEGNOM",
    };
    for (int i = 0; i < kNumCandidates; ++i) {
        const char* method = kCandidateNames[i];
        for (const char* cls : kClasses) {
            void* target = SafeResolveMethod(cls, method, 1);
            if (target) {
                g_autoFireFns[i] = reinterpret_cast<SetAutoFireFn>(target);
                DBG_FILE_LOG("[AimHooks] set_AutoFire candidate resolved: " << method << " on class " << cls);
                if (!g_setAutoFireFn) {
                    g_setAutoFireFn = g_autoFireFns[i];
                }
                break;
            }
        }
    }
}

static void* AutoFireThisPtr()
{
    void* shootThis = s_lastSwaThis.load(std::memory_order_acquire);
    if (AddrOk(shootThis)) return shootThis;

    void* local = GameState::GetLocalPtr();
    if (!AddrOk(local)) return nullptr;

    Il2CppClass* fireKlass = Resolver::GetClass("", "AAGOIIEJOMO");
    if (!fireKlass) fireKlass = Resolver::FindClassLoose("AAGOIIEJOMO");
    if (!fireKlass) fireKlass = Resolver::GetClass("", kPlayerFireCls);
    if (!fireKlass) fireKlass = Resolver::FindClassLoose(kPlayerFireCls);
    if (!fireKlass) return local;

    Il2CppClass* localKlass = nullptr;
    __try {
        localKlass = reinterpret_cast<Il2CppClass*>(*reinterpret_cast<void**>(local));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return local;
    }
    if (!localKlass) return local;

    __try {
        void* fieldIter = nullptr;
        for (FieldInfo* field; (field = il2cpp_class_get_fields(localKlass, &fieldIter)) != nullptr; ) {
            if (!field->type) continue;
            const Il2CppClass* fieldKlass = il2cpp_class_from_type(field->type);
            if (!fieldKlass) continue;
            if (fieldKlass != fireKlass
                && !il2cpp_class_is_assignable_from(fireKlass, const_cast<Il2CppClass*>(fieldKlass)))
                continue;
            const size_t off = static_cast<size_t>(field->offset);
            if (off == 0 || off > 0x2000) continue;
            void* candidate = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(local) + off);
            if (AddrOk(candidate)) return candidate;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    return local;
}

static SetAutoFireFn GetActiveSetAutoFireFn()
{
    int idx = s_activeAutoFireCandidateIdx.load(std::memory_order_relaxed);
    if (idx >= 0 && idx < kNumCandidates) {
        return g_autoFireFns[idx];
    }
    return g_setAutoFireFn;
}

static void SyncNativeAutoFire(bool on)
{
    SetAutoFireFn fn = GetActiveSetAutoFireFn();
    if (!fn) return;
    void* self = AutoFireThisPtr();
    if (!AddrOk(self)) return;
    const bool cur = s_nativeAutoFireOn.load(std::memory_order_relaxed);
    if (cur == on) return;
    __try {
        fn(self, on, nullptr);
        s_nativeAutoFireOn.store(on, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

} // namespace

namespace AimHooks {

bool Install()
{
    if (s_installed) return true;

    g_csaTarget = SafeResolveMethod(kPlayerClass, kCSAMethod, 4);
    if (!g_csaTarget)
        g_csaTarget = SafeResolveMethod(kShootClass, kCSAMethod, 4);
    g_swaTarget = SafeResolveMethod(kPlayerFireCls, kSWAMethod, 1);
    if (!g_swaTarget)
        g_swaTarget = SafeResolveMethod(kShootClass,  kSWAMethod, 1);
    if (!g_swaTarget)
        g_swaTarget = SafeResolveMethod(kPlayerClass, kSWAMethod, 1);
    g_sspTarget = SafeResolveMethod(kShootClass,  kSSPMethod, 2);
    if (!g_csaTarget || !g_swaTarget) {
        static int s_failN = 0;
        if ((s_failN++ % 120) == 0)
            DBG_FILE_LOG("[AimHooks] Install FAILED csa=" << (g_csaTarget != nullptr)
                << " swa=" << (g_swaTarget != nullptr)
                << " (attempt=" << s_failN << ")");
        return false;
    }

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

    if (kEnableSendShotPacketHook && g_sspTarget) {
        if (MH_CreateHook(g_sspTarget, reinterpret_cast<void*>(&SendShotPacketDetour),
                          reinterpret_cast<void**>(&g_sspOrig)) != MH_OK) return false;
    }

    if (MH_QueueEnableHook(g_csaTarget) != MH_OK) return false;
    if (MH_QueueEnableHook(g_swaTarget) != MH_OK) return false;
    if (kEnableSendShotPacketHook && g_sspTarget)
        MH_QueueEnableHook(g_sspTarget);
    if (MH_ApplyQueued() != MH_OK) return false;

    s_installed = true;
    DBG_FILE_LOG("[AimHooks] Install OK — CSA+SWA hooks"
        << (kEnableSendShotPacketHook ? " (+SSP)" : " (SSP disabled)")
        << " csa=" << g_csaTarget << " swa=" << g_swaTarget);
    return true;
}

void Uninstall()
{
    if (!s_installed) return;
    ReleaseAutoFire();
    s_aimActive.store(false, std::memory_order_release);
    s_hasTarget.store(false, std::memory_order_release);
    if (g_csaTarget) { MH_DisableHook(g_csaTarget); MH_RemoveHook(g_csaTarget); }
    if (g_swaTarget) { MH_DisableHook(g_swaTarget); MH_RemoveHook(g_swaTarget); }
    if (g_sspTarget && kEnableSendShotPacketHook) {
        MH_DisableHook(g_sspTarget); MH_RemoveHook(g_sspTarget);
    }
    g_csaOrig = nullptr; g_swaOrig = nullptr; g_sspOrig = nullptr;
    g_csaTarget = g_swaTarget = g_sspTarget = nullptr;
    s_installed = false;
}

bool IsInstalled() { return s_installed; }

void SetAimActive(bool on)
{
    s_aimActive.store(on, std::memory_order_release);
}

void SetTarget(bool hasTarget, float x, float y)
{
    s_hasTarget.store(hasTarget, std::memory_order_release);
    s_targetX.store(x, std::memory_order_release);
    s_targetY.store(y, std::memory_order_release);
}

void SetReverseCultStaff(bool v)   { s_reverseCultStaff.store(v, std::memory_order_relaxed); }
void SetOffsetColossusSword(bool v) { s_offsetColossus.store(v, std::memory_order_relaxed); }

HookStats GetHookStats()
{
    HookStats s{};
    s.csaCalls    = s_csaCalls.load(std::memory_order_relaxed);
    s.csaRedirect = s_csaRedirect.load(std::memory_order_relaxed);
    s.swaCalls    = s_swaCalls.load(std::memory_order_relaxed);
    s.swaRedirect = s_swaRedirect.load(std::memory_order_relaxed);
    return s;
}

void UpdateAutoFire(bool wantFire)
{
    ResolveSetAutoFireOnce();

    if (!wantFire || !s_aimActive.load(std::memory_order_acquire)
        || !s_hasTarget.load(std::memory_order_acquire)
        || !s_installed)
    {
        SyncNativeAutoFire(false);
        return;
    }

    if (g_setAutoFireFn)
        SyncNativeAutoFire(true);

    // Do not call TryInvokeShoot — synthetic in-game shots send malformed PLAYERSHOOT
    // and cause server kick. Outbound aim is handled by proxy raw-byte patch.
}

void ReleaseAutoFire()
{
    s_lastAutoFireMs = 0;
    SyncNativeAutoFire(false);
}

uint64_t GetAutoFireShots()
{
    return s_autoFireShots.load(std::memory_order_relaxed);
}

const char* GetAutoFireCandidateName(int idx)
{
    if (idx >= 0 && idx < kNumCandidates) return kCandidateNames[idx];
    return nullptr;
}

bool IsAutoFireCandidateResolved(int idx)
{
    if (idx >= 0 && idx < kNumCandidates) return g_autoFireFns[idx] != nullptr;
    return false;
}

void TestCallAutoFireCandidate(int idx, bool on)
{
    if (idx < 0 || idx >= kNumCandidates) return;
    SetAutoFireFn fn = g_autoFireFns[idx];
    if (!fn) return;
    void* self = AutoFireThisPtr();
    if (!AddrOk(self)) return;
    __try {
        fn(self, on, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int GetActiveAutoFireCandidateIdx()
{
    return s_activeAutoFireCandidateIdx.load(std::memory_order_relaxed);
}

void SetActiveAutoFireCandidateIdx(int idx)
{
    s_activeAutoFireCandidateIdx.store(idx, std::memory_order_relaxed);
}

} // namespace AimHooks
