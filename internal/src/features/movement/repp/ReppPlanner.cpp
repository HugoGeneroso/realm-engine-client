#include "pch-il2cpp.h"
#include "ReppPlanner.h"
#include "ReppSensors.h"
#include "ReppField.h"

#include <algorithm>
#include <cmath>

namespace RePP { namespace Planner {
namespace {

// ── Tuned constants (baked, not exposed — keeps the knob surface small) ──────
constexpr float kTimingPadMs    = 15.f;
constexpr float kArrivalHoldMs  = 120.f;
constexpr float kPlayerRadius   = 0.05f;   // near-point; CCD (M4) tightens further
constexpr float kClearanceTiles = 0.03f;
constexpr float kEnemyAvoidance = 0.50f;   // no-go pad beyond an enemy body
constexpr float kEscapeEpsilon  = 0.01f;
constexpr float kMinCorrection  = 0.12f;   // smallest ring radius (tiles)
constexpr float kBackpedalW     = 3.0f;
constexpr float kPerpW          = 5.0f;
constexpr float kIntentW        = 2.5f;
constexpr float kClearanceW     = 1.5f;
constexpr float kCorrectionW    = 1.2f;    // prefer the smallest slide that works
constexpr int   kSweepSteps     = 6;

float ClampF(float v, float lo, float hi) { return std::clamp(v, lo, hi); }

int ThreatCount(const SensorSnapshot& s)  { return std::clamp(s.threatCount, 0, kMaxThreats); }
int BlockerCount(const SensorSnapshot& s) { return std::clamp(s.blockerCount, 0, kMaxBlockers); }
int SampleCount(const Threat& t)          { return std::clamp(t.sampleCount, 0, kMaxPathSamples); }

float HitScale(const Settings& s) { return ClampF(s.hitScale, 0.5f, 2.f); }

float EffThreatHalf(const Threat& t, const Settings& s)
{
    return std::max(0.f, t.radius * HitScale(s)) + kPlayerRadius + kClearanceTiles;
}

float EffEnemyHalf(const Blocker& b)
{
    return std::max(0.f, b.radius) + kPlayerRadius + kClearanceTiles + kEnemyAvoidance;
}

void ThreatBounds(const Threat& t, Vec2& mn, Vec2& mx)
{
    if (t.hasBounds) { mn = t.boundsMin; mx = t.boundsMax; return; }
    const int n = SampleCount(t);
    if (n <= 0) { mn = {}; mx = {}; return; }
    mn = mx = t.samples[0];
    for (int i = 1; i < n; ++i) {
        mn.x = std::min(mn.x, t.samples[i].x); mn.y = std::min(mn.y, t.samples[i].y);
        mx.x = std::max(mx.x, t.samples[i].x); mx.y = std::max(mx.y, t.samples[i].y);
    }
}

bool BoundsCanHit(const Threat& t, Vec2 p, float half)
{
    Vec2 mn{}, mx{};
    ThreatBounds(t, mn, mx);
    return p.x >= mn.x - half && p.x <= mx.x + half && p.y >= mn.y - half && p.y <= mx.y + half;
}

float SampleTimeMs(const Threat& t, int i)
{
    const float v = t.sampleTimesMs[std::clamp(i, 0, kMaxPathSamples - 1)];
    return std::isfinite(v) && v >= 0.f ? v : 0.f;
}

bool TimeOverlaps(float s0, float s1, float arrivalMs, float holdMs)
{
    const float lo = std::max(0.f, arrivalMs - kTimingPadMs);
    const float hi = arrivalMs + std::max(holdMs, 0.f) + kTimingPadMs;
    if (s0 > s1) std::swap(s0, s1);
    return s0 <= hi && s1 >= lo;
}

bool ClipToWindow(float s0, float s1, float arrivalMs, float holdMs, float& t0, float& t1)
{
    const float lo = std::max(0.f, arrivalMs - kTimingPadMs);
    const float hi = arrivalMs + std::max(holdMs, 0.f) + kTimingPadMs;
    if (s0 > s1) std::swap(s0, s1);
    if (s0 > hi || s1 < lo) return false;
    const float denom = s1 - s0;
    if (denom <= 1e-4f) { t0 = t1 = 0.f; return true; }
    t0 = std::clamp((std::max(s0, lo) - s0) / denom, 0.f, 1.f);
    t1 = std::clamp((std::min(s1, hi) - s0) / denom, 0.f, 1.f);
    return t0 <= t1;
}

Vec2 ClosestOnSeg(Vec2 p, Vec2 a, Vec2 b)
{
    const Vec2 ab = Sub(b, a);
    const float d = LenSq(ab);
    if (d < 1e-6f) return a;
    const float t = std::clamp(Dot(Sub(p, a), ab) / d, 0.f, 1.f);
    return Add(a, Mul(ab, t));
}

bool ThreatHitsAt(const Threat& t, Vec2 p, const Settings& s, float arrivalMs, float holdMs)
{
    const float half = EffThreatHalf(t, s);
    if (!BoundsCanHit(t, p, half)) return false;
    const int n = SampleCount(t);
    for (int i = 0; i < n; ++i) {
        const float sMs = SampleTimeMs(t, i);
        if (TimeOverlaps(sMs, sMs, arrivalMs, holdMs) && ChebDistance(p, t.samples[i]) <= half) return true;
        if (i + 1 < n) {
            const float nMs = SampleTimeMs(t, i + 1);
            float a = 0.f, b = 0.f;
            if (ClipToWindow(sMs, nMs, arrivalMs, holdMs, a, b)) {
                const Vec2 pa = Add(t.samples[i], Mul(Sub(t.samples[i + 1], t.samples[i]), a));
                const Vec2 pb = Add(t.samples[i], Mul(Sub(t.samples[i + 1], t.samples[i]), b));
                if (ChebDistance(p, ClosestOnSeg(p, pa, pb)) <= half) return true;
            }
        }
    }
    return false;
}

float ThreatClearance(Vec2 p, float arrivalMs, float holdMs, const Settings& s, const SensorSnapshot& sn)
{
    float best = 9999.f;
    for (int i = 0; i < ThreatCount(sn); ++i) {
        const Threat& t = sn.threats[i];
        const float half = EffThreatHalf(t, s);
        if (!BoundsCanHit(t, p, half)) continue;
        const int n = SampleCount(t);
        for (int j = 0; j < n; ++j) {
            if (!TimeOverlaps(SampleTimeMs(t, j), SampleTimeMs(t, j), arrivalMs, holdMs)) continue;
            best = std::min(best, ChebDistance(p, t.samples[j]) - half);
        }
    }
    return best;
}

bool EnemyHits(const Blocker& b, Vec2 p) { return ChebDistance(p, b.pos) <= EffEnemyHalf(b); }

float EnemyClearance(Vec2 p, const SensorSnapshot& sn)
{
    float best = 9999.f;
    for (int i = 0; i < BlockerCount(sn); ++i)
        best = std::min(best, ChebDistance(p, sn.blockers[i].pos) - EffEnemyHalf(sn.blockers[i]));
    return best;
}

float Clearance(Vec2 p, float arrivalMs, float holdMs, const Settings& s, const SensorSnapshot& sn)
{
    float best = std::min(EnemyClearance(p, sn), ThreatClearance(p, arrivalMs, holdMs, s, sn));
    return best == 9999.f ? 10.f : best;
}

// Generalized escape rule: if the player currently overlaps a danger (enemy body
// or hazard ground) and the candidate strictly reduces that overlap, the move is
// allowed even though the candidate still registers as "inside".
bool EscapingEnemy(const Blocker& b, Vec2 player, Vec2 cand)
{
    if (!EnemyHits(b, player)) return false;
    return ChebDistance(cand, b.pos) > ChebDistance(player, b.pos) + kEscapeEpsilon;
}

CandidateRejectReason RejectAt(Vec2 cand, Vec2 player, float arrivalMs, float holdMs,
                               const Settings& s, const SensorSnapshot& sn, bool hazardBlocks)
{
    if (Sensors::IsWallAt(cand.x, cand.y)) return CandidateRejectReason::Wall;
    // Hazard blocks endpoints (never STOP on damaging ground) and intermediate
    // sweep points when we are NOT already escaping a hazard. While sliding off a
    // hazard we stand on, passing through hazard cells is tolerated (hazardBlocks
    // is set false for those intermediate points by the caller).
    if (hazardBlocks && s.avoidHazards && Sensors::IsHazardAt(cand.x, cand.y))
        return CandidateRejectReason::Hazard;
    for (int i = 0; i < BlockerCount(sn); ++i) {
        const Blocker& b = sn.blockers[i];
        if (!EnemyHits(b, cand)) continue;
        if (EscapingEnemy(b, player, cand)) continue;
        return CandidateRejectReason::Enemy;
    }
    for (int i = 0; i < ThreatCount(sn); ++i)
        if (ThreatHitsAt(sn.threats[i], cand, s, arrivalMs, holdMs)) return CandidateRejectReason::Projectile;
    return CandidateRejectReason::None;
}

// ── Threat flow (for the perpendicular-sidestep bias) ───────────────────────
struct Flow { Vec2 dir{}; float coherence = 0.f; bool has = false; };

Flow ComputeFlow(const SensorSnapshot& sn)
{
    Vec2 sum{};
    float total = 0.f;
    for (int i = 0; i < ThreatCount(sn); ++i) {
        const Threat& t = sn.threats[i];
        const int n = SampleCount(t);
        if (n < 2) continue;
        const Vec2 d = Normalize(Sub(t.samples[n - 1], t.samples[0]));
        if (LenSq(d) <= 1e-4f) continue;
        const float w = std::max(1.f, t.damage * 0.01f);
        sum = Add(sum, Mul(d, w));
        total += w;
    }
    Flow f{};
    if (total <= 0.f) return f;
    const float mag = Len(sum);
    f.coherence = std::clamp(mag / total, 0.f, 1.f);
    f.has = mag > 1e-4f;
    f.dir = f.has ? Mul(sum, 1.f / mag) : Vec2{};
    return f;
}

Vec2 ComputeSlideDir(Vec2 intentDir, Vec2 player, const SensorSnapshot& sn)
{
    Vec2 adj = intentDir;
    for (int i = 0; i < BlockerCount(sn); ++i) {
        const Blocker& b = sn.blockers[i];
        const Vec2 away = Normalize(Sub(player, b.pos));
        const float inward = Dot(adj, Mul(away, -1.f));
        const float r = EffEnemyHalf(b);
        if (LenSq(Sub(player, b.pos)) <= r * r * 1.44f && inward > 0.f)
            adj = Add(adj, Mul(away, inward));
    }
    return Normalize(adj);
}

float SpeedTilesPerMs(float moveBudget, float frameMs)
{
    return frameMs > 0.f ? std::max(1e-4f, moveBudget / frameMs) : 1e-4f;
}

} // namespace

bool IsSweepSafe(Vec2 from, Vec2 to, const Settings& settings, const SensorSnapshot& sensors,
                 float moveBudget, float frameMs)
{
    const float dist = Len(Sub(to, from));
    // Time the sweep against the REAL traversal speed (per-frame move budget),
    // not the path length — otherwise the whole path is modelled as ~one frame
    // and mid-path projectile crossings are missed.
    const float speed = SpeedTilesPerMs(moveBudget, std::max(1.f, frameMs));
    const bool onHazard = settings.avoidHazards && Sensors::IsHazardAt(from.x, from.y);
    for (int i = 1; i <= kSweepSteps; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(kSweepSteps);
        const Vec2 p = Add(from, Mul(Sub(to, from), f));
        const float arrivalMs = dist > 0.f ? (dist * f) / speed : 0.f;
        const bool endpoint = (i == kSweepSteps);
        const bool hazardBlocks = endpoint ? true : !onHazard;
        if (RejectAt(p, from, arrivalMs, 0.f, settings, sensors, hazardBlocks) != CandidateRejectReason::None)
            return false;
    }
    return true;
}

PlanResult Evaluate(const PlanRequest& req)
{
    PlanResult out{};
    const Settings& s = req.settings;
    const SensorSnapshot& sn = req.sensors;
    const Vec2 player = req.player;
    const float maxMove = ClampF(std::isfinite(s.maxMoveTiles) ? s.maxMoveTiles : 1.f, 0.2f, 4.f);
    const float moveBudget = std::clamp(std::isfinite(req.moveBudget) ? req.moveBudget : 0.f, 0.f, maxMove);
    const float frameMs = std::isfinite(req.frameMs) && req.frameMs > 0.f ? std::clamp(req.frameMs, 1.f, 250.f) : 16.667f;
    const float reactMs = ClampF(std::isfinite(s.reactWindowMs) ? s.reactWindowMs : 650.f, 100.f, 2500.f);
    const float speed = SpeedTilesPerMs(moveBudget, frameMs);
    const float dangerScale = ClampF(s.dangerWeight, 0.f, 5.f);

    const bool playerOnHazard = s.avoidHazards && Sensors::IsHazardAt(player.x, player.y);
    const int threats = ThreatCount(sn);
    const int blockers = BlockerCount(sn);

    if (threats == 0 && blockers == 0 && !playerOnHazard) {
        out.status = FrameStatus::NoThreats;
        return out;
    }

    const bool hasIntent = LenSq(req.intentDir) > 1e-4f;
    const Vec2 intentDir = Normalize(req.intentDir);
    const Vec2 intended = Add(player, Mul(intentDir, hasIntent ? moveBudget : 0.f));

    // Intent gate — do nothing if the player's own move (or standing still) is
    // safe over the reaction window AND not on / into a wall or hazard.
    // Intent gate. The player's intended move is "safe" only if it ends on
    // non-hazard, non-wall, threat-clear ground — IsSweepSafe blocks a hazard
    // endpoint and, when the player starts on a hazard, tolerates transit hazard
    // while escaping. So if the player is already walking off a vent on a safe
    // heading we hand control back instead of hijacking it.
    bool intentSafe = false;
    if (hasIntent) {
        const float arrMs = moveBudget > 0.f ? Len(Sub(intended, player)) / speed : 0.f;
        intentSafe = IsSweepSafe(player, intended, s, sn, moveBudget, frameMs) &&
                     RejectAt(intended, player, arrMs, kArrivalHoldMs, s, sn, /*hazardBlocks*/true) == CandidateRejectReason::None;
    } else if (!playerOnHazard) {
        intentSafe = RejectAt(player, player, 0.f, reactMs, s, sn, /*hazardBlocks*/true) == CandidateRejectReason::None;
    }
    if (intentSafe) {
        // Autopilot actively pursues its goal even when the move is safe; Assist
        // hands control straight back to the player when their move is safe.
        if (hasIntent && s.mode == static_cast<int>(Mode::Autopilot)) {
            out.status = FrameStatus::IntentSafe;
            out.target = intended;
            out.shouldMove = true;
            return out;
        }
        out.status = hasIntent ? FrameStatus::IntentSafe : FrameStatus::NoThreats;
        return out;
    }

    out.slideDir = ComputeSlideDir(hasIntent ? intentDir : Vec2{}, player, sn);
    const Flow flow = ComputeFlow(sn);

    // Ring sampler: direction × radius pass. Pick the highest-scoring SAFE
    // candidate; if none is safe, the least-bad (max-clearance) fallback.
    Candidate best{};      float bestScore = -1e9f; bool found = false;
    Candidate fallback{};  float fbScore  = -1e9f;  bool haveFb = false;

    auto consider = [&](Vec2 dir, float dist) {
        const Vec2 cand = Add(player, Mul(dir, dist));
        const float arrMs = dist / speed;
        Candidate c{};
        c.pos = cand;
        c.tauMs = arrMs;
        const bool sweepSafe = IsSweepSafe(player, cand, s, sn, moveBudget, frameMs);
        const CandidateRejectReason endReason = RejectAt(cand, player, arrMs, kArrivalHoldMs, s, sn, /*hazardBlocks*/true);
        c.safe = sweepSafe && endReason == CandidateRejectReason::None;
        c.rejectReason = !sweepSafe ? CandidateRejectReason::Sweep : endReason;

        const float clr = Clearance(cand, arrMs, kArrivalHoldMs, s, sn);
        const float intentDot = hasIntent ? Dot(dir, intentDir) : 0.f;
        float perp = 0.f;
        if (flow.has) perp = (1.f - 2.f * std::fabs(Dot(dir, flow.dir))) * flow.coherence;
        c.score = std::clamp(clr, -2.f, 4.f) * kClearanceW * std::max(0.25f, dangerScale)
                + intentDot * kIntentW
                + perp * kPerpW
                - dist * kCorrectionW
                + (intentDot < 0.f ? intentDot * kBackpedalW : 0.f);

        if (out.candidateCount < kMaxCandidates) out.candidates[out.candidateCount++] = c;
        if (c.safe) {
            if (!found || c.score > bestScore) { best = c; bestScore = c.score; found = true; }
        } else if (!Sensors::IsWallAt(cand.x, cand.y)) {
            // Hazard-aware least-bad fallback: strongly prefer an off-hazard
            // endpoint, then projectile/enemy clearance; when escaping a hazard
            // prefer the candidate that makes the most progress off the source
            // tile (so a fully-flooded room still steers toward the edge).
            const bool candHazard = s.avoidHazards && Sensors::IsHazardAt(cand.x, cand.y);
            const float fScore = (candHazard ? 0.f : 1000.f)
                               + std::clamp(clr, -2.f, 10.f) * 2.f
                               + (playerOnHazard ? Len(Sub(cand, player)) * 3.f : 0.f);
            if (!haveFb || fScore > fbScore) { fallback = c; fbScore = fScore; haveFb = true; }
        }
    };

    // Hold (stay) is only a candidate when the player isn't already in danger
    // on their tile; otherwise standing still is never an option.
    if (!playerOnHazard) consider(Vec2{ 0.f, 0.f }, 0.f);

    // When standing on a hazard, look farther than the assist cap — the nearest
    // safe tile may be >1 away. The per-frame step stays clamped to the move
    // budget downstream, so a distant target just sets the escape heading.
    const float searchMax = playerOnHazard ? std::max(maxMove, 2.0f) : maxMove;
    for (int pass = 0; pass < kRingPasses; ++pass) {
        const float f = kRingPasses > 1 ? static_cast<float>(pass) / static_cast<float>(kRingPasses - 1) : 1.f;
        const float dist = kMinCorrection + (searchMax - kMinCorrection) * f;
        for (int d = 0; d < kCandidateDirs; ++d) {
            const float ang = kTwoPi * static_cast<float>(d) / static_cast<float>(kCandidateDirs);
            consider(Vec2{ std::cos(ang), std::sin(ang) }, dist);
        }
    }

    if (found) {
        if (LenSq(Sub(best.pos, player)) <= 1e-6f) { out.status = FrameStatus::NoThreats; return out; }
        out.status = FrameStatus::CandidateAssist;
        out.target = best.pos;
        out.shouldMove = true;
        return out;
    }

    // No safe straight-line candidate (boxed in / wall between us and safety).
    // Run the multi-step field search, which routes AROUND walls/obstacles to the
    // nearest safe pocket (the cure for the confined-space "stuck" feedback).
    {
        const Field::EscapeResult esc = Field::FindEscape(req);
        if (esc.found) {
            out.slideDir = esc.firstDir;
            out.status = FrameStatus::FieldEscape;
            out.target = esc.target;
            out.shouldMove = true;
            return out;
        }
    }

    if (haveFb) {
        out.status = FrameStatus::SlideAssist;
        out.target = fallback.pos;
        out.shouldMove = true;
        return out;
    }
    out.status = FrameStatus::NoSafeCandidate;
    return out;
}

bool CellSafeToStand(Vec2 cell, Vec2 player, float arrivalMs, float holdMs,
                     const Settings& settings, const SensorSnapshot& sensors)
{
    return RejectAt(cell, player, arrivalMs, holdMs, settings, sensors, /*hazardBlocks*/true)
           == CandidateRejectReason::None;
}

float ArrivalSpeed(float moveBudget, float frameMs)
{
    return SpeedTilesPerMs(moveBudget, frameMs);
}

} } // namespace RePP::Planner

