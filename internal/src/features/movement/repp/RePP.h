#pragma once

namespace RePP {

void SetEnabled(bool enabled);
bool IsEnabled();
void OnEnter();
void Tick(void* player, float px, float py, float dt);
void RenderSettings();
void RenderDebugOverlay(float camX, float camY, float angle, float zoom, float cx, float cy);

// M0 knobs (stored only; NO movement behavior yet):
void  SetReactWindowMs(float ms);   float GetReactWindowMs();   // default 650, clamp [100,2500]
void  SetMaxMoveTiles(float tiles); float GetMaxMoveTiles();    // default 1.0, clamp [0.2,4]
void  SetHitScale(float s);         float GetHitScale();        // default 1.0, clamp [0.5,2]
void  SetDangerWeight(float v);     float GetDangerWeight();    // default 2.0, clamp [0,5]
void  SetMode(int mode);            int   GetMode();            // 0=Assist (default), 1=Autopilot
void  SetAvoidHazards(bool en);     bool  GetAvoidHazards();    // default true
void  SetDebugOverlay(bool en);     bool  GetDebugOverlay();    // default true
void  SetFollowLantern(bool en);    bool  GetFollowLantern();   // Autopilot stand-on scan (default OFF, perf)
void  SetStandOnType(int t);        int   GetStandOnType();     // Autopilot stand-on objType (0=off)

// ── Live diagnostics view (DiagBridge / external runtime tests) ──────────────
// Flat snapshot of the current dodge frame — plain scalars only, so the diag
// egress can serialize it without pulling in ReppTypes. Thread-safe (locks the
// same mutex PublishDebug uses).
struct DiagView {
    int   status;             // FrameStatus: 0=Disabled,2=NoThreats,…,9=SensorLimited
    bool  enabled;
    float playerX, playerY;
    bool  hasSelectedTarget;  float selX, selY;   // aim target
    bool  hasLockTarget;      float lockX, lockY; // Autopilot boss lock
    int   candidateCount;
    int   threatCount;        // projectiles/AoE the sensor saw this frame
    int   blockerCount;       // wall/enemy blockers
    float tileSpeedAtPlayer;
};
DiagView GetDiagView();

} // namespace RePP
