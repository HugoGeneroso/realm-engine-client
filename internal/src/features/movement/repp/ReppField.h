#pragma once

#include "ReppTypes.h"

// RE++ field search (R1 + R2). A short-horizon Dijkstra over a local grid that
// routes AROUND walls and obstacles to the nearest safe-to-stand pocket, scored
// with arrival-time danger. Invoked by the planner only when the straight-line
// ring sampler is boxed in — the cure for the "stuck in confined spaces" /
// "walks into walls" feedback that single-direction sampling can't solve.
namespace RePP { namespace Field {

struct EscapeResult {
    bool found = false;
    Vec2 target{};    // world-space first waypoint to move toward
    Vec2 firstDir{};  // unit heading of the first step (debug / slide)
};

EscapeResult FindEscape(const PlanRequest& req);

} } // namespace RePP::Field

