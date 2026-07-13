#include "pch-il2cpp.h"
#include "core/runtime/WorldGate.h"
#include "core/runtime/GameState.h"
#include "features/combat/autoaim/AutoAim.h"
#include "features/movement/noclip/NoclipHook.h"
#include "features/movement/dodge/ProjectileTracking.h"
#include "features/movement/dodge/AoeTracking.h"
#include "features/combat/autoaim/ProjNoclip.h"
#include "features/movement/dodge/DangerPlanner.h"
#include "features/account/CredentialCapture.h"
#include "core/logging/DbgFileLog.h"

#include <atomic>

namespace {

constexpr ULONGLONG kSettleMs = 3000ULL;

std::atomic<ULONGLONG> s_changeTick{ 0 };
std::atomic<bool>        s_pendingChange{ false };

} // namespace

namespace WorldGate {

    void OnLocalPtrChanged()
    {
        const ULONGLONG now = GetTickCount64();
        s_changeTick.store(now, std::memory_order_relaxed);
        s_pendingChange.store(true, std::memory_order_relaxed);
    }

    bool ConsumeWorldChange()
    {
        bool expected = true;
        return s_pendingChange.compare_exchange_strong(
            expected, false, std::memory_order_acq_rel);
    }

    bool IsSettling()
    {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG t = s_changeTick.load(std::memory_order_relaxed);
        if (t == 0) return false;
        return (now - t) < kSettleMs;
    }

    void OnWorldChange()
    {
        DbgFileLogWrite("[WorldGate] world change — uninstalling hooks/writers");
        AutoAim::Uninstall();
        NoclipHook::Uninstall();
        ProjectileTracking::Uninstall();
        AoeTracking::Uninstall();
        ProjNoclip::Uninstall();
        DangerPlanner::Uninstall();
        CredentialCapture::Uninstall();
    }

} // namespace WorldGate
