#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

// RE++ shared types. One home for the engine's constants, settings, sensor
// snapshot, candidate, and plan structs. Pure data — no game/IL2CPP includes.
namespace RePP {

// ── Capacities (fixed-size buffers — zero per-frame heap allocation) ─────────
constexpr int   kMaxThreats       = 128;  // projectiles + AoE
constexpr int   kMaxPathSamples   = 24;   // time-parametrized samples per threat
constexpr int   kMaxBlockers      = 64;   // live enemy/boss bodies (culled by range)
constexpr int   kCandidateDirs    = 32;   // M2 ring-sampler directions
constexpr int   kRingPasses       = 4;    // M2 radius passes (small → max correction)
constexpr int   kMaxCandidates    = kCandidateDirs * kRingPasses + 1; // +1 = hold
constexpr float kTwoPi            = 6.28318530717958647692f;

enum class Mode : int {
    Assist    = 0,   // intent-preserving slide-assist (default)
    Autopilot = 1,   // full survival / orbit / flee
};

// Per-frame outcome, surfaced to the debug overlay.
enum class FrameStatus : uint8_t {
    Disabled,
    NoPlayer,
    NoThreats,        // nothing dangerous → no action
    IntentSafe,       // player's own move is safe → no action
    SlideAssist,      // slid the unsafe component around danger
    CandidateAssist,  // picked a ring candidate
    FieldEscape,      // multi-step field search produced the route (M3)
    NoSafeCandidate,  // boxed in — least-bad fallback
    MovementFailed,   // native MoveTo / resolve failed
    SensorLimited,    // a sensor source was unavailable/capped this frame
};

enum class CandidateRejectReason : uint8_t {
    None,
    Projectile,
    Enemy,
    Wall,
    Hazard,
    Sweep,
    OutsideBudget,
};

struct Vec2 {
    float x = 0.f;
    float y = 0.f;
};

// Shared inline vector math — one definition, used by every RE++ translation
// unit (zDodge duplicated these across two files; we don't).
inline float Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float LenSq(Vec2 v)       { return Dot(v, v); }
inline Vec2  Add(Vec2 a, Vec2 b) { return { a.x + b.x, a.y + b.y }; }
inline Vec2  Sub(Vec2 a, Vec2 b) { return { a.x - b.x, a.y - b.y }; }
inline Vec2  Mul(Vec2 v, float s){ return { v.x * s, v.y * s }; }
inline float Len(Vec2 v)         { return std::sqrt(LenSq(v)); }
inline Vec2  Normalize(Vec2 v)   { const float n = Len(v); return n > 1e-4f ? Mul(v, 1.f / n) : Vec2{}; }
inline float ChebDistance(Vec2 a, Vec2 b) { return std::max(std::fabs(a.x - b.x), std::fabs(a.y - b.y)); }

// Exposed tunables (the ~6-knob surface; everything else is a tuned constant).
struct Settings {
    float reactWindowMs = 650.f;   // prediction horizon for projectile checks
    float maxMoveTiles  = 1.0f;    // max correction distance per assist
    float hitScale      = 1.0f;    // projectile AABB multiplier (1.0 = exact)
    float dangerWeight  = 2.0f;    // aggressiveness: lower = threads tighter
    int   mode          = static_cast<int>(Mode::Assist);
    bool  avoidHazards  = true;    // N1: treat damaging ground as danger
    bool  debugOverlay  = true;
    bool  followLantern = false;   // Autopilot: enable the stand-on scan (default
                                   // OFF — it iterates the world entity list,
                                   // which costs perf; only on for the lantern)
    int   standOnType   = 0;       // objType to STAND ON when followLantern is on
                                   // (e.g. the Moonlight Village lantern)
};

// A moving hazard (enemy projectile or AoE), sampled along its predicted path.
// Times are ms from "now" (sample 0 == current live position, t≈0).
struct Threat {
    int32_t id = 0;
    float   radius = 0.10f;   // collision half-size (tiles), pre hit-scale
    float   damage = 0.f;
    int     sampleCount = 0;
    Vec2    samples[kMaxPathSamples]{};
    float   sampleTimesMs[kMaxPathSamples]{};
    Vec2    boundsMin{};
    Vec2    boundsMax{};
    bool    hasBounds = false;
};

// A live body the player must not stand on (contact damage). Walls and hazard
// tiles are NOT materialized here — the planner queries the cached WorldTAB tile
// maps (IsTileBlocked / IsDamagingTile) at the exact points it evaluates, which
// is both cheaper and always-current vs a per-frame grid scan.
struct Blocker {
    enum class Kind : uint8_t { Enemy } kind = Kind::Enemy;
    int32_t id = 0;
    Vec2    pos{};
    float   radius = 0.5f;
};

struct SensorSnapshot {
    Threat  threats[kMaxThreats]{};
    int     threatCount = 0;
    Blocker blockers[kMaxBlockers]{};
    int     blockerCount = 0;
    float   tileSpeedAtPlayer = 1.f;  // N2: ground speed multiplier under player
    bool    projectileSourceUnavailable = false;
    bool    projectileLimited = false;
    bool    aoeLimited = false;
    bool    blockerLimited = false;
    // Autopilot boss lock, computed in the sensor's game-thread enemy pass
    // (fresh + race-free; no WorldTAB::GetEntities needed for the boss).
    bool    hasLock = false;
    int32_t lockId = 0;
    Vec2    lockPos{};
};

// One sampled correction target (debug + planner bookkeeping).
struct Candidate {
    Vec2  pos{};
    float score = 0.f;
    float tauMs = 0.f;   // time-to-first-collision along this heading
    bool  safe = false;
    CandidateRejectReason rejectReason = CandidateRejectReason::None;
};

struct PlanRequest {
    Settings      settings{};
    SensorSnapshot sensors{};
    Vec2  player{};
    Vec2  intentDir{};   // unit WASD/autopilot direction (zero = none)
    float moveBudget = 0.f;
    float frameMs = 16.667f;
};

struct PlanResult {
    FrameStatus status = FrameStatus::NoThreats;
    Vec2  target{};
    Vec2  slideDir{};
    bool  shouldMove = false;
    Candidate candidates[kMaxCandidates]{};
    int   candidateCount = 0;
};

// Published to the overlay each frame (read on the render thread).
struct DebugSnapshot {
    FrameStatus status = FrameStatus::Disabled;
    Vec2  player{};
    Vec2  intentDir{};
    Vec2  intendedTarget{};
    Vec2  slideDir{};
    Vec2  selectedTarget{};
    bool  hasSelectedTarget = false;
    Candidate candidates[kMaxCandidates]{};
    int   candidateCount = 0;
    SensorSnapshot sensors{};
    Vec2  lockTarget{};            // Autopilot locked enemy (biggest targetable)
    bool  hasLockTarget = false;
};

} // namespace RePP

