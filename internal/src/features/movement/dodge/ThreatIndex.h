#pragma once

#include <vector>

// ThreatIndex — broad-phase over predicted projectile trajectories for the
// RolloutDodge forward simulation. Each rebuild, every tracked bullet is
// reduced to a Threat: its center position sampled at fixed sub-step times
// over the planning horizon, its effective collision half-side, and the
// axis-aligned box its whole sampled path sweeps. The rollout then, per
// candidate input, queries the bullets whose swept box overlaps the player's
// swept path and runs precise per-sample CCD only on those.
//
// Two backends sit behind this interface:
//   * BruteForceIndex   — linear AABB scan. The always-correct reference; used
//                         for small bullet counts and as a cross-check.
//   * GridThreatIndex   (separate TU) — uniform spatial hash; the primary
//                         broad-phase for dense fields. Dep-free, MSVC-clean.
// (A quadtree was considered but rejected: on Realm's dense, dynamic, small-
// resolution field the per-rebuild build cost swamps the query savings.)

namespace Threats {

// Max sub-step samples cached per bullet trajectory. horizon/sampleMs must
// stay <= this; the planner clamps to it.
inline constexpr int kMaxThreatSamples = 96;

struct Aabb {
    float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
};

inline bool AabbOverlap(const Aabb& a, const Aabb& b)
{
    return a.minX <= b.maxX && a.maxX >= b.minX &&
           a.minY <= b.maxY && a.maxY >= b.minY;
}

// One bullet's precomputed horizon trajectory + collision half-side. cx/cy are
// the predicted center at t = k * sampleMs after the rebuild instant; nSamples
// is how many leading entries are valid (the bullet may expire mid-horizon).
struct Threat {
    int   bulletIdx = -1;
    int   nSamples  = 0;
    float half      = 0.f;       // effR (bullet half * scale + player half + pad)
    float cx[kMaxThreatSamples];
    float cy[kMaxThreatSamples];
    Aabb  box;                   // swept AABB over the valid samples, incl. half
};

// Broad-phase interface. Build() ingests this rebuild's threats (it only needs
// each threat's .box + its index in the vector); Query() appends the indices of
// threats whose box overlaps `region`. Build + all Query calls happen
// synchronously within one RolloutDodge::Tick on the game-update thread, so the
// backend may hold a pointer to the caller's threat vector for the rebuild.
class ThreatIndex {
public:
    virtual ~ThreatIndex() = default;
    virtual void Build(const std::vector<Threat>& threats) = 0;
    virtual void Query(const Aabb& region, std::vector<int>& out) const = 0;
    virtual const char* Name() const = 0;
};

// Always-correct linear baseline: O(N) AABB scan, no structure. Cheap enough
// for small N, and the reference the grid backend is cross-checked against.
class BruteForceIndex final : public ThreatIndex {
public:
    void Build(const std::vector<Threat>& threats) override { m_threats = &threats; }
    void Query(const Aabb& region, std::vector<int>& out) const override
    {
        if (!m_threats) return;
        const std::vector<Threat>& t = *m_threats;
        for (int i = 0; i < static_cast<int>(t.size()); ++i)
            if (AabbOverlap(t[i].box, region))
                out.push_back(i);
    }
    const char* Name() const override { return "brute"; }
private:
    const std::vector<Threat>* m_threats = nullptr;
};

} // namespace Threats
