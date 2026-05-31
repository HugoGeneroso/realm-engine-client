#include "pch-il2cpp.h"
#include "GridThreatIndex.h"

#include <algorithm>

namespace Threats {

namespace {
constexpr float kCellTiles = 1.0f;   // base cell size (tiles)
constexpr int   kMaxDim    = 96;     // cap grid dims; cell grows if exceeded
}

int GridThreatIndex::CellX(float wx) const
{
    int c = static_cast<int>((wx - m_originX) * m_invCell);
    if (c < 0) c = 0;
    if (c >= m_w) c = m_w - 1;
    return c;
}
int GridThreatIndex::CellY(float wy) const
{
    int c = static_cast<int>((wy - m_originY) * m_invCell);
    if (c < 0) c = 0;
    if (c >= m_h) c = m_h - 1;
    return c;
}

void GridThreatIndex::Build(const std::vector<Threat>& threats)
{
    m_threats = &threats;
    m_w = m_h = 0;
    const int n = static_cast<int>(threats.size());
    if (n == 0) return;

    // Bounds over all threat swept-AABBs.
    float minX = threats[0].box.minX, minY = threats[0].box.minY;
    float maxX = threats[0].box.maxX, maxY = threats[0].box.maxY;
    for (int i = 1; i < n; ++i) {
        const Aabb& b = threats[i].box;
        minX = std::min(minX, b.minX); minY = std::min(minY, b.minY);
        maxX = std::max(maxX, b.maxX); maxY = std::max(maxY, b.maxY);
    }

    // Grow the cell so the grid stays within kMaxDim even if a far threat
    // stretches the bounds (keeps the cell array bounded).
    float cell = kCellTiles;
    const float spanX = std::max(0.f, maxX - minX);
    const float spanY = std::max(0.f, maxY - minY);
    const float grow  = std::max({ 1.f,
        spanX / (static_cast<float>(kMaxDim) * cell),
        spanY / (static_cast<float>(kMaxDim) * cell) });
    cell *= grow;

    m_originX = minX; m_originY = minY;
    m_invCell = 1.f / cell;
    m_w = std::clamp(static_cast<int>(spanX / cell) + 1, 1, kMaxDim);
    m_h = std::clamp(static_cast<int>(spanY / cell) + 1, 1, kMaxDim);

    const int cellCount = m_w * m_h;
    if (static_cast<int>(m_cells.size()) < cellCount) m_cells.resize(cellCount);
    for (int i = 0; i < cellCount; ++i) m_cells[i].clear();

    m_seen.assign(static_cast<size_t>(n), 0u);
    m_gen = 0;

    for (int i = 0; i < n; ++i) {
        const Aabb& b = threats[i].box;
        const int x0 = CellX(b.minX), x1 = CellX(b.maxX);
        const int y0 = CellY(b.minY), y1 = CellY(b.maxY);
        for (int cy = y0; cy <= y1; ++cy)
            for (int cx = x0; cx <= x1; ++cx)
                m_cells[cy * m_w + cx].push_back(i);
    }
}

void GridThreatIndex::Query(const Aabb& region, std::vector<int>& out) const
{
    if (m_w == 0 || !m_threats) return;
    const int x0 = CellX(region.minX), x1 = CellX(region.maxX);
    const int y0 = CellY(region.minY), y1 = CellY(region.maxY);
    ++m_gen;
    const std::vector<Threat>& th = *m_threats;
    for (int cy = y0; cy <= y1; ++cy) {
        for (int cx = x0; cx <= x1; ++cx) {
            const std::vector<int>& bucket = m_cells[cy * m_w + cx];
            for (int idx : bucket) {
                if (m_seen[idx] == m_gen) continue;     // already emitted this query
                m_seen[idx] = m_gen;
                if (AabbOverlap(th[idx].box, region))   // cell membership is coarse
                    out.push_back(idx);
            }
        }
    }
}

} // namespace Threats
