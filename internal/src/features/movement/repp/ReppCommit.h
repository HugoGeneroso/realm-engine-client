#pragma once

#include "ReppTypes.h"

// RE++ commit layer (R4). CCD-exact tight commit: re-check the ACTUAL per-frame
// step at fine time resolution and shorten it if the full step would clip a
// projectile/enemy/hazard, catching grazes the coarser path sweep missed (the
// "doesn't fully dodge the fast shots" feedback). Reuses the planner's shared
// collision model — no duplicated geometry.
namespace RePP { namespace Commit {

Vec2 Refine(Vec2 player, Vec2 moveTarget, const Settings& settings,
            const SensorSnapshot& sensors, float moveBudget, float frameMs);

} } // namespace RePP::Commit

