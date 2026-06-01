#include "pch-il2cpp.h"
#include "ZaclinDodgeDebug.h"

#include "W2S.h"

#include <imgui/imgui.h>
#include <algorithm>
#include <cmath>

namespace ZaclinDodge::Debug {
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

void DrawCircle(ImDrawList* draw, Vec2 p, float radiusPx, ImU32 color, float camX, float camY, float angle, float zoom, float cx, float cy)
{
    ImVec2 screen;
    if (ToScreen(p, camX, camY, angle, zoom, cx, cy, screen)) draw->AddCircle(screen, radiusPx, color, 16, 1.5f);
}

void DrawLine(ImDrawList* draw, Vec2 a, Vec2 b, ImU32 color, float camX, float camY, float angle, float zoom, float cx, float cy, float thickness = 1.5f)
{
    ImVec2 sa, sb;
    if (ToScreen(a, camX, camY, angle, zoom, cx, cy, sa) && ToScreen(b, camX, camY, angle, zoom, cx, cy, sb))
        draw->AddLine(sa, sb, color, thickness);
}

const char* StatusName(FrameStatus status)
{
    switch (status) {
    case FrameStatus::Disabled: return "Disabled";
    case FrameStatus::NoPlayer: return "No player";
    case FrameStatus::NoThreats: return "No threats";
    case FrameStatus::IntentSafe: return "Intent safe";
    case FrameStatus::SlideAssist: return "Slide assist";
    case FrameStatus::CandidateAssist: return "Candidate assist";
    case FrameStatus::NoSafeCandidate: return "No safe candidate";
    case FrameStatus::MovementFailed: return "Movement failed";
    case FrameStatus::SensorLimited: return "Sensor limited";
    default: return "Unknown";
    }
}

} // namespace

void Render(const DebugSnapshot& snapshot, const Settings& settings, float camX, float camY, float angle, float zoom, float cx, float cy)
{
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (!draw) return;
    const ImU32 projectileColor = IM_COL32(255, 80, 80, 220);
    const ImU32 pathColor = IM_COL32(255, 180, 80, 160);
    const ImU32 enemyColor = IM_COL32(255, 60, 200, 210);
    const ImU32 obstacleColor = IM_COL32(90, 160, 255, 190);
    const ImU32 safeColor = IM_COL32(80, 255, 140, 190);
    const ImU32 rejectColor = IM_COL32(255, 80, 80, 120);
    const ImU32 intentColor = IM_COL32(255, 255, 255, 230);
    const ImU32 slideColor = IM_COL32(80, 255, 255, 230);
    const ImU32 targetColor = IM_COL32(80, 255, 80, 255);
    const ImU32 textColor = IM_COL32(245, 245, 245, 230);

    const SensorSnapshot& sensors = snapshot.sensors;
    const int threatCount = std::clamp(sensors.threatCount, 0, kMaxThreats);
    for (int i = 0; i < threatCount; ++i) {
        const Threat& threat = sensors.threats[i];
        const int sampleCount = std::clamp(threat.sampleCount, 0, kMaxPathSamples);
        for (int s = 0; s < sampleCount; ++s) {
            DrawCircle(draw, threat.samples[s], 3.f, projectileColor, camX, camY, angle, zoom, cx, cy);
            if (s + 1 < sampleCount)
                DrawLine(draw, threat.samples[s], threat.samples[s + 1], pathColor, camX, camY, angle, zoom, cx, cy, 1.f);
        }
    }

    const int blockerCount = std::clamp(sensors.blockerCount, 0, kMaxBlockers);
    for (int i = 0; i < blockerCount; ++i) {
        const Blocker& blocker = sensors.blockers[i];
        DrawCircle(draw, blocker.pos, blocker.kind == Blocker::Kind::Enemy ? 8.f : 5.f,
            blocker.kind == Blocker::Kind::Enemy ? enemyColor : obstacleColor,
            camX, camY, angle, zoom, cx, cy);
    }

    if (settings.candidateOverlay) {
        const int candidateCount = std::clamp(snapshot.candidateCount, 0, kMaxCandidates);
        for (int i = 0; i < candidateCount; ++i) {
            const CandidateDebug& c = snapshot.candidates[i];
            DrawCircle(draw, c.pos, 2.5f, c.safe ? safeColor : rejectColor, camX, camY, angle, zoom, cx, cy);
        }
    }

    if (snapshot.status != FrameStatus::Disabled && snapshot.status != FrameStatus::NoPlayer) {
        DrawLine(draw, snapshot.player, snapshot.intendedTarget, intentColor, camX, camY, angle, zoom, cx, cy, 2.f);
        DrawLine(draw, snapshot.player, { snapshot.player.x + snapshot.slideDir.x, snapshot.player.y + snapshot.slideDir.y }, slideColor, camX, camY, angle, zoom, cx, cy, 2.f);
    }
    if (snapshot.hasSelectedTarget) DrawCircle(draw, snapshot.selectedTarget, 7.f, targetColor, camX, camY, angle, zoom, cx, cy);

    ImVec2 playerScreen;
    if (ToScreen(snapshot.player, camX, camY, angle, zoom, cx, cy, playerScreen))
        draw->AddText(ImVec2(playerScreen.x + 10.f, playerScreen.y - 18.f), textColor, StatusName(snapshot.status));
}

} // namespace ZaclinDodge::Debug