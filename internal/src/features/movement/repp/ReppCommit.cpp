#include "pch-il2cpp.h"
#include "ReppCommit.h"
#include "ReppPlanner.h"   // Planner::CellSafeToStand, Planner::ArrivalSpeed

#include <algorithm>
#include <cmath>

namespace RePP { namespace Commit {
namespace {

constexpr int kSubsteps = 8;   // fine sub-frame resolution (vs the planner's path sweep)

// True if the swept player motion from->to (over its real traversal time) stays
// clear of every projectile/enemy/hazard at fine resolution.
bool SweptClear(Vec2 from, Vec2 to, float speed, const Settings& s, const SensorSnapshot& sn)
{
    const float dist = Len(Sub(to, from));
    for (int i = 1; i <= kSubsteps; ++i) {
        const float frac = static_cast<float>(i) / static_cast<float>(kSubsteps);
        const Vec2 p = Add(from, Mul(Sub(to, from), frac));
        const float t = speed > 0.f ? (dist * frac) / speed : 0.f;
        // The endpoint must be dwell-safe (match the planner's arrival-hold), not
        // just instantaneously clear — a shortened step shouldn't stop somewhere
        // a bullet sweeps into a moment later.
        const float holdMs = (i == kSubsteps) ? 120.f : 0.f;
        if (!Planner::CellSafeToStand(p, from, t, holdMs, s, sn)) return false;
    }
    return true;
}

} // namespace

Vec2 Refine(Vec2 player, Vec2 moveTarget, const Settings& settings,
            const SensorSnapshot& sensors, float moveBudget, float frameMs)
{
    const Vec2 step = Sub(moveTarget, player);
    if (LenSq(step) <= 1e-8f) return moveTarget;

    const float speed = Planner::ArrivalSpeed(moveBudget, frameMs);

    // Try the full step first; only shorten if it would clip. The longest
    // clip-free fraction wins (stay as close to the plan as safely possible).
    static constexpr float kFractions[] = { 1.0f, 0.8f, 0.6f, 0.4f, 0.2f };
    for (float f : kFractions) {
        const Vec2 to = Add(player, Mul(step, f));
        if (SweptClear(player, to, speed, settings, sensors)) return to;
    }
    // Every shortened step still clips — keep the planner's choice (best effort).
    return moveTarget;
}

} } // namespace RePP::Commit

