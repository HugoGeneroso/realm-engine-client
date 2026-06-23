#pragma once

#include "ReppTypes.h"

// RE++ sensor layer. Builds one bounded SensorSnapshot per tick (projectiles +
// AoE as time-parametrized threats, live enemy bodies as blockers) and exposes
// O(1) cached-tile-map queries the planner uses for walls and damaging ground.
//
// Design vs zDodge: walls/hazards are NOT materialized as a per-frame grid scan.
// The planner asks IsWallAt / IsHazardAt only at the exact points it evaluates,
// against WorldTAB's already-cached blocked/damaging maps — cheaper and always
// current. N1 (hazard ground) and N2 (tile speed) enter here.
namespace RePP { namespace Sensors {

// Snapshot the dynamic world (projectiles, AoE, enemies) around the player.
SensorSnapshot Build(float playerX, float playerY, const Settings& settings);

// ── Cached tile-map queries (hitbox-aware where relevant) ───────────────────
// True if the player centre at (worldX, worldY) collides with a wall /
// FullOccupy constraint (Flash isValidPosition parity, via TestTAB).
bool  IsWallAt(float worldX, float worldY);
// N1: true if the tile under (worldX, worldY) deals ground damage.
bool  IsHazardAt(float worldX, float worldY);
// N2: ground speed multiplier under (worldX, worldY); 1.0 when unmodified.
float TileSpeedAt(float worldX, float worldY);

} } // namespace RePP::Sensors

