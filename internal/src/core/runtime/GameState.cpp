#include "pch-il2cpp.h"
#include "GameState.h"
#include "RuntimeOffsets.h"
#include "Il2CppResolver.h"
#include "DbgFileLog.h"

#include <Windows.h>

// ─────────────────────────────────────────────────────────────────────────────
// GameState — see GameState.h for design notes.
// ─────────────────────────────────────────────────────────────────────────────

namespace GameState {

static Il2CppClass* s_appMgrClass      = nullptr;
static void*        s_appMgr           = nullptr;  // cached after first successful find
static void*        s_worldMgr         = nullptr;  // re-read every Tick
static void*        s_localPtr         = nullptr;  // re-read every Tick
static ULONGLONG    s_lastAppMgrTry    = 0;        // rate-limits the expensive FindObjectsByType
static ULONGLONG    s_appMgrDeferUntil = 0;        // skip FindObjectsOfType during login/menu
static bool         s_wmOffsetResolved = false;    // true once AppMgr_WorldMgr is dynamically set
static constexpr ULONGLONG kAppMgrDeferMs = 20000ULL;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static inline bool AddrOk(const void* p)
{
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    return a >= 0x10000u && a <= 0x7FFFFFFFFFFFull;
}

// SEH must live in a function without C++ unwinding (C2712).
static void* ReadPtr(const void* base, size_t offset) noexcept
{
    __try {
        return *reinterpret_cast<void* const*>(
            reinterpret_cast<const uint8_t*>(base) + offset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Cheap vtable sanity: first qword of the object should be a valid code address.
static bool PtrOk(const void* p)
{
    if (!AddrOk(p)) return false;
    __try {
        void* klass = *reinterpret_cast<void* const*>(p);
        return AddrOk(klass);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void Tick()
{
    // ── Step 1: resolve ApplicationManager once ──────────────────────────────
    // FindObjectsByType is expensive (full IL2CPP object walk).
    // Rate-limited to one attempt every 5 s until it succeeds.
    if (!AddrOk(s_appMgr))
    {
        const ULONGLONG now = GetTickCount64();
        // UnityEngine.Object.FindObjectsOfType on the render thread during login/menu
        // has hung/crashed the process (~3s freeze then UnityCrashHandler).
        if (!s_appMgrDeferUntil) {
            s_appMgrDeferUntil = now + kAppMgrDeferMs;
            DBG_FILE_LOG("[GameState] AppMgr lookup deferred "
                << kAppMgrDeferMs << "ms (avoid FindObjectsOfType during login)");
            return;
        }
        if (now < s_appMgrDeferUntil) return;

        if (s_lastAppMgrTry != 0 && now - s_lastAppMgrTry < 5000ULL) return;
        s_lastAppMgrTry = now;

        if (!s_appMgrClass)
        {
            s_appMgrClass = Resolver::FindClass("", "ApplicationManager");
            if (!s_appMgrClass)
                s_appMgrClass = Resolver::FindClassLoose("ApplicationManager");
        }
        if (!s_appMgrClass) return;

        auto objs = Resolver::FindObjectsByType(s_appMgrClass);
        if (objs.empty()) return;

        void* candidate = objs[0];
        if (!PtrOk(candidate)) return;
        s_appMgr = candidate;
    }

    // ── Step 1b: resolve AppMgr→WorldMgr offset via type-scan (once) ────────
    // il2cpp_field_get_offset on a C# property backing field returns the dump
    // offset; no ACTK shift for ApplicationManager fields.
    // We find the field by its declared type (HJMBOMEHGDJ) rather than its name
    // because the backing-field name (<CHDFAEBMILI>k__BackingField) changes with
    // every BeeByte obfuscation pass.
    if (!s_wmOffsetResolved && s_appMgrClass)
    {
        Il2CppClass* wmClass = Resolver::FindClassLoose("HJMBOMEHGDJ");
        if (wmClass)
        {
            void* iter = nullptr;
            FieldInfo* fi;
            while ((fi = il2cpp_class_get_fields(s_appMgrClass, &iter)) != nullptr)
            {
                const Il2CppType* ft = il2cpp_field_get_type(fi);
                if (!ft) continue;
                Il2CppClass* fklass = il2cpp_class_from_type(ft);
                if (fklass == wmClass)
                {
                    RuntimeOffsets::AppMgr_WorldMgr =
                        static_cast<uint32_t>(il2cpp_field_get_offset(fi));
                    break;
                }
            }
            s_wmOffsetResolved = true; // mark done whether or not found; fallback stays if not
        }
    }

    // ── Step 2: WorldMgr — 1 deref per frame ─────────────────────────────────
    void* wm = ReadPtr(s_appMgr, RuntimeOffsets::AppMgr_WorldMgr);
    if (!AddrOk(wm)) { s_worldMgr = nullptr; s_localPtr = nullptr; return; }
    s_worldMgr = wm;

    // ── Step 3: LocalPtr — prefer WM_Local; keep NotifyLocalPtr / entity-scan
    // fallback across brief null windows during realm transitions (WM_Local often
    // lags one+ frames behind the entity dict).
    void* lp = ReadPtr(wm, RuntimeOffsets::WM_Local);
    if (PtrOk(lp)) {
        s_localPtr = lp;
    } else if (s_localPtr && !PtrOk(s_localPtr)) {
        s_localPtr = nullptr;
    }
}

void NotifyLocalPtr(void* ptr)
{
    if (PtrOk(ptr)) s_localPtr = ptr;
}

void* GetAppMgr()   { return s_appMgr;   }
void* GetWorldMgr() { return s_worldMgr; }
void* GetLocalPtr() { return s_localPtr; }

} // namespace GameState
