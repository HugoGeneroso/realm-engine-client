#include "pch-il2cpp.h"
#include "NoclipHook.h"
#include "Noclip.h"
#include "Il2CppResolver.h"
#include "minhook/MinHook.h"

#include <Windows.h>
#include <cstdint>

namespace {

static inline bool AddrOk(const void* p)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    return a > 0x10000u && a < 0x7FFFFFFFFFFFull;
}

static bool EnsureIl2CppThreadAttached()
{
    static thread_local bool attached = false;
    if (attached)
        return true;
    if (!il2cpp_domain_get || !il2cpp_thread_attach)
        return false;

    Il2CppDomain* domain = il2cpp_domain_get();
    if (!domain)
        return false;

    attached = il2cpp_thread_attach(domain) != nullptr;
    return attached;
}

static const MethodInfo* FindMethod(Il2CppClass* klass, const char* name, int argc)
{
    const MethodInfo* method = nullptr;
    Resolver::Protection::safe_call([&]() {
        method = klass ? il2cpp_class_get_method_from_name(klass, name, argc) : nullptr;
    });
    return method && method->methodPointer ? method : nullptr;
}

static Il2CppClass* FindClassAny(const char* namespaze, const char* name)
{
    Il2CppClass* klass = nullptr;
    Resolver::Protection::safe_call([&]() {
        klass = Resolver::FindClass(namespaze, name);
        if (!klass)
            klass = Resolver::FindClassLoose(name);
        if (!klass)
            klass = Resolver::GetClass(namespaze, name);
    });
    return klass;
}

template <typename Fn>
static void AssignMethod(Fn& target, Il2CppClass* klass, const char* methodName)
{
    const MethodInfo* method = FindMethod(klass, methodName, 2);
    if (method && AddrOk(method->methodPointer))
        target = reinterpret_cast<Fn>(method->methodPointer);
}

static decltype(app::HJMBOMEHGDJ_PEGDEDNHEHD) s_origPegdednhehd = nullptr;
static decltype(app::HJMBOMEHGDJ_LHGGJIAKLMJ) s_origLhggjiaklmj = nullptr;

static bool s_resolved = false;
static bool s_installed = false;
static void* s_pegTarget = nullptr;
static void* s_lhgTarget = nullptr;

static bool dHJMBOMEHGDJ_PEGDEDNHEHD(app::HJMBOMEHGDJ* self, float x, float y, MethodInfo* method)
{
    if (Noclip::ShouldBypassWalkable())
        return true;
    return s_origPegdednhehd ? s_origPegdednhehd(self, x, y, method) : false;
}

static bool dHJMBOMEHGDJ_LHGGJIAKLMJ(app::HJMBOMEHGDJ* self, float x, float y, MethodInfo* method)
{
    if (Noclip::ShouldBypassWalkable())
        return true;
    return s_origLhggjiaklmj ? s_origLhggjiaklmj(self, x, y, method) : false;
}

static void ResolveTargets()
{
    if (s_resolved)
        return;
    if (!EnsureIl2CppThreadAttached())
        return;

    Il2CppClass* mapViewService = FindClassAny("", "HJMBOMEHGDJ");
    if (!mapViewService)
        return;

    AssignMethod(app::HJMBOMEHGDJ_PEGDEDNHEHD, mapViewService, "PEGDEDNHEHD");
    AssignMethod(app::HJMBOMEHGDJ_LHGGJIAKLMJ, mapViewService, "LHGGJIAKLMJ");

    s_pegTarget = reinterpret_cast<void*>(app::HJMBOMEHGDJ_PEGDEDNHEHD);
    s_lhgTarget = reinterpret_cast<void*>(app::HJMBOMEHGDJ_LHGGJIAKLMJ);

    s_resolved = AddrOk(s_pegTarget) && AddrOk(s_lhgTarget);
}

static bool EnsureMinHook()
{
    static bool mhInitialized = false;
    if (mhInitialized)
        return true;

    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        return false;

    mhInitialized = true;
    return true;
}

static void TryInstall()
{
    if (s_installed)
        return;

    ResolveTargets();
    if (!s_resolved || !EnsureMinHook())
        return;

    if (MH_CreateHook(s_pegTarget,
            reinterpret_cast<void*>(&dHJMBOMEHGDJ_PEGDEDNHEHD),
            reinterpret_cast<void**>(&s_origPegdednhehd)) != MH_OK)
        return;

    if (MH_EnableHook(s_pegTarget) != MH_OK) {
        MH_RemoveHook(s_pegTarget);
        s_origPegdednhehd = nullptr;
        return;
    }

    if (MH_CreateHook(s_lhgTarget,
            reinterpret_cast<void*>(&dHJMBOMEHGDJ_LHGGJIAKLMJ),
            reinterpret_cast<void**>(&s_origLhggjiaklmj)) != MH_OK) {
        MH_DisableHook(s_pegTarget);
        MH_RemoveHook(s_pegTarget);
        s_origPegdednhehd = nullptr;
        return;
    }

    if (MH_EnableHook(s_lhgTarget) != MH_OK) {
        MH_DisableHook(s_pegTarget);
        MH_RemoveHook(s_pegTarget);
        MH_RemoveHook(s_lhgTarget);
        s_origPegdednhehd = nullptr;
        s_origLhggjiaklmj = nullptr;
        return;
    }

    s_installed = true;
}

} // namespace

namespace NoclipHook {

void Tick()
{
    if (!Noclip::IsEnabled())
        return;
    TryInstall();
}

void Uninstall()
{
    if (!s_installed)
        return;

    if (s_lhgTarget) {
        MH_DisableHook(s_lhgTarget);
        MH_RemoveHook(s_lhgTarget);
        s_lhgTarget = nullptr;
    }
    if (s_pegTarget) {
        MH_DisableHook(s_pegTarget);
        MH_RemoveHook(s_pegTarget);
        s_pegTarget = nullptr;
    }

    s_origPegdednhehd = nullptr;
    s_origLhggjiaklmj = nullptr;
    s_installed = false;
}

bool IsInstalled()
{
    return s_installed;
}

bool IsResolved()
{
    ResolveTargets();
    return s_resolved;
}

} // namespace NoclipHook
