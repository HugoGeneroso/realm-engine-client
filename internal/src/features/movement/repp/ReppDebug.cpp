#include "pch-il2cpp.h"
#include "ReppDebug.h"

#include "W2S.h"

#include <imgui/imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace RePP { namespace Debug {
namespace {

bool ToScreen(Vec2 p, float camX, float camY, float angle, float zoom, float cx, float cy, ImVec2& out)
{
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(camX) || !std::isfinite(camY) ||
        !std::isfinite(angle) || !std::isfinite(zoom) || !std::isfinite(cx) || !std::isfinite(cy) || zoom <= 0.f)
        return false;
    float sx = 0.f, sy = 0.f;
    if (!W2S(p.x, p.y, sx, sy, camX, camY, angle, zoom, cx, cy)) return false;
    if (!std::isfinite(sx) || !std::isfinite(sy)) return false;
    out = ImVec2(sx, sy);
    return true;
}

void DrawDot(ImDrawList* d, Vec2 p, float r, ImU32 col, float camX, float camY, float angle, float zoom, float cx, float cy)
{
    ImVec2 s;
    if (ToScreen(p, camX, camY, angle, zoom, cx, cy, s)) d->AddCircleFilled(s, r, col, 12);
}

void DrawLine(ImDrawList* d, Vec2 a, Vec2 b, ImU32 col, float camX, float camY, float angle, float zoom, float cx, float cy, float th)
{
    ImVec2 sa, sb;
    if (ToScreen(a, camX, camY, angle, zoom, cx, cy, sa) && ToScreen(b, camX, camY, angle, zoom, cx, cy, sb))
        d->AddLine(sa, sb, col, th);
}

ImU32 RejectColor(CandidateRejectReason r, bool safe)
{
    if (safe) return IM_COL32(60, 230, 90, 200);          // green = safe
    switch (r) {
        case CandidateRejectReason::Projectile:    return IM_COL32(235, 60, 60, 180);   // red
        case CandidateRejectReason::Enemy:         return IM_COL32(230, 60, 230, 180);  // magenta
        case CandidateRejectReason::Wall:          return IM_COL32(140, 140, 140, 160); // grey
        case CandidateRejectReason::Hazard:        return IM_COL32(240, 150, 30, 200);  // orange
        case CandidateRejectReason::Sweep:         return IM_COL32(230, 220, 60, 170);  // yellow
        case CandidateRejectReason::OutsideBudget: return IM_COL32(90, 120, 200, 150);  // blue
        default:                                   return IM_COL32(180, 180, 180, 150);
    }
}

const char* StatusText(FrameStatus s)
{
    switch (s) {
        case FrameStatus::Disabled:        return "off";
        case FrameStatus::NoPlayer:        return "no-player";
        case FrameStatus::NoThreats:       return "clear";
        case FrameStatus::IntentSafe:      return "intent-safe";
        case FrameStatus::SlideAssist:     return "slide";
        case FrameStatus::CandidateAssist: return "assist";
        case FrameStatus::FieldEscape:     return "field-escape";
        case FrameStatus::NoSafeCandidate: return "boxed-in";
        case FrameStatus::MovementFailed:  return "move-fail";
        case FrameStatus::SensorLimited:   return "sensor-limited";
    }
    return "?";
}

} // namespace

void Render(const DebugSnapshot& snap, const Settings& settings,
            float camX, float camY, float angle, float zoom, float cx, float cy)
{
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    if (!d) return;

    // Status banner.
    char buf[96];
    std::snprintf(buf, sizeof(buf), "RE++ [%s]  threats:%d  blockers:%d  cands:%d",
                  StatusText(snap.status), snap.sensors.threatCount, snap.sensors.blockerCount, snap.candidateCount);
    d->AddText(ImVec2(12.f, 12.f), IM_COL32(120, 220, 255, 255), buf);

    // Threat predicted paths.
    for (int i = 0; i < std::min(snap.sensors.threatCount, kMaxThreats); ++i) {
        const Threat& t = snap.sensors.threats[i];
        const int n = std::min(t.sampleCount, kMaxPathSamples);
        for (int j = 0; j + 1 < n; ++j)
            DrawLine(d, t.samples[j], t.samples[j + 1], IM_COL32(235, 80, 80, 110), camX, camY, angle, zoom, cx, cy, 1.5f);
        if (n > 0) DrawDot(d, t.samples[0], 3.f, IM_COL32(255, 90, 90, 220), camX, camY, angle, zoom, cx, cy);
    }

    // Enemy blockers.
    for (int i = 0; i < std::min(snap.sensors.blockerCount, kMaxBlockers); ++i)
        DrawDot(d, snap.sensors.blockers[i].pos, 4.f, IM_COL32(230, 60, 230, 150), camX, camY, angle, zoom, cx, cy);

    // Candidates (toggle-gated by the candidate overlay being part of debug).
    for (int i = 0; i < std::min(snap.candidateCount, kMaxCandidates); ++i) {
        const Candidate& c = snap.candidates[i];
        DrawDot(d, c.pos, 2.5f, RejectColor(c.rejectReason, c.safe), camX, camY, angle, zoom, cx, cy);
    }

    // Intent (cyan) and slide (white) vectors.
    if (LenSq(snap.intentDir) > 1e-4f)
        DrawLine(d, snap.player, snap.intendedTarget, IM_COL32(60, 220, 220, 200), camX, camY, angle, zoom, cx, cy, 2.f);
    if (LenSq(snap.slideDir) > 1e-4f)
        DrawLine(d, snap.player, Add(snap.player, snap.slideDir), IM_COL32(240, 240, 240, 200), camX, camY, angle, zoom, cx, cy, 1.5f);

    // Selected target.
    if (snap.hasSelectedTarget) {
        DrawDot(d, snap.selectedTarget, 5.f, IM_COL32(255, 210, 0, 230), camX, camY, angle, zoom, cx, cy);
        DrawLine(d, snap.player, snap.selectedTarget, IM_COL32(255, 210, 0, 160), camX, camY, angle, zoom, cx, cy, 2.f);
    }

    // Autopilot lock target (the biggest targetable enemy being orbited).
    if (snap.hasLockTarget) {
        ImVec2 s;
        if (ToScreen(snap.lockTarget, camX, camY, angle, zoom, cx, cy, s)) {
            d->AddCircle(s, 13.f, IM_COL32(255, 70, 70, 230), 16, 2.f);
            d->AddLine(ImVec2(s.x - 16.f, s.y), ImVec2(s.x + 16.f, s.y), IM_COL32(255, 70, 70, 200), 1.5f);
            d->AddLine(ImVec2(s.x, s.y - 16.f), ImVec2(s.x, s.y + 16.f), IM_COL32(255, 70, 70, 200), 1.5f);
        }
    }

    (void)settings;
}

} } // namespace RePP::Debug

