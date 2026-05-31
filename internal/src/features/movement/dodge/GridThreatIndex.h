#pragma once

#include "ThreatIndex.h"
#include <vector>
#include <cstdint>

namespace Threats {

// GridThreatIndex — uniform spatial-hash broad-phase. A local grid spans the
// threats' bounds each rebuild; Build() stamps every threat's swept-AABB cells,
// Query() gathers threats from the cells overlapping the region (deduped by a
// per-query generation stamp, then exact-AABB rechecked).
//
// This is the structure that fits Realm's dense, dynamic, small-resolution
// field: O(1) build-per-insert, no tree split/merge, buffers reused across
// rebuilds (no per-frame heap churn). It's a pure prune behind ThreatIndex —
// the brute-force backend stays the correctness reference, and RolloutDodge
// only selects the grid when the bullet count is high enough to pay for it.
class GridThreatIndex final : public ThreatIndex {
public:
    void Build(const std::vector<Threat>& threats) override;
    void Query(const Aabb& region, std::vector<int>& out) const override;
    const char* Name() const override { return "grid"; }

private:
    const std::vector<Threat>*    m_threats = nullptr;
    float                         m_originX = 0.f, m_originY = 0.f;
    float                         m_invCell = 1.f;     // 1 / cellSize
    int                           m_w = 0, m_h = 0;
    std::vector<std::vector<int>> m_cells;             // bucket per cell (reused)
    mutable std::vector<uint32_t> m_seen;              // per-threat dedup stamp
    mutable uint32_t              m_gen = 0;

    int CellX(float wx) const;
    int CellY(float wy) const;
};

} // namespace Threats
