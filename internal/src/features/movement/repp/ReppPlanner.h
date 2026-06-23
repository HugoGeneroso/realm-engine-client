#pragma once

#include "ReppTypes.h"

// RE++ planner — pure(ish) PlanRequest → PlanResult. The intent gate +
// ring-sampler (direction × radius passes) + frictionless slide + the
// escape-overlap rule (generalized from enemies to hazard tiles). Wall and
// hazard tests defer to ReppSensors' cached tile-map queries.
namespace RePP { namespace Planner {

PlanResult Evaluate(const PlanRequest& req);

// True if the straight sweep from `from` to `to` stays clear of projectiles,
// walls, hazards, and enemy bodies. Used by the commit-dwell layer.
bool IsSweepSafe(Vec2 from, Vec2 to, const Settings& settings,
                 const SensorSnapshot& sensors, float moveBudget, float frameMs);

// ── Shared collision model (reused by the M3 field search / M4 commit so they
//    never duplicate the threat geometry) ────────────────────────────────────
// True if a player standing at `cell` for `holdMs` starting at `arrivalMs` would
// be clear of walls, hazard ground, enemy bodies, and projectiles. `player` is
// the live position (for the escape-overlap allowance).
bool  CellSafeToStand(Vec2 cell, Vec2 player, float arrivalMs, float holdMs,
                      const Settings& settings, const SensorSnapshot& sensors);
// Per-ms travel speed used for arrival-time math (tiles/ms, floored > 0).
float ArrivalSpeed(float moveBudget, float frameMs);

} } // namespace RePP::Planner

