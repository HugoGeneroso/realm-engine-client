#pragma once

#include <algorithm>
#include <cmath>

// DodgeSpeed — realized-speed estimator shared by the dodge engines. Mirrors
// XDodge's anti-rubber-band obsSpeed EMA: we observe the motion the game
// actually granted between ticks (the true post-clamp / post-status speed) and
// roll it into an EMA, used as the per-step movement budget for the planner.
// Pure observation of the (px,py,dt) the Tick already receives — no game call.
// Stateful: one instance per engine.
namespace DodgeSpeed {

enum class Sample { Stationary, Learned, Teleport };

struct ObsSpeed {
    float spd      = 5.0f;   // tiles/sec EMA, clamped to RotMG's real band
    bool  havePrev = false;
    float prevX    = 0.f;
    float prevY    = 0.f;

    // Feed one frame's live position + wall-clock dt. Returns whether we
    // learned from motion, stood still, or saw a teleport-sized jump (realm
    // change) so the caller can drop stale per-type caches.
    Sample Update(float px, float py, float dt)
    {
        Sample r = Sample::Stationary;
        if (havePrev && dt > 1e-3f) {
            const float ddx = px - prevX, ddy = py - prevY;
            const float inst = std::sqrt(ddx * ddx + ddy * ddy) / dt;
            // Only learn from MEANINGFUL motion — standing still must not decay
            // spd toward 0 (that makes arrival-time over-cautious and laggy
            // when you then move). Teleport-sized jumps = realm change.
            if (inst >= 1.0f && inst < 25.f) {
                spd += 0.20f * (inst - spd);
                r = Sample::Learned;
            } else if (inst >= 25.f) {
                r = Sample::Teleport;
            }
            spd = std::clamp(spd, 2.0f, 12.0f);
        }
        prevX = px; prevY = py; havePrev = true;
        return r;
    }

    void Reset() { havePrev = false; spd = 5.0f; prevX = 0.f; prevY = 0.f; }
};

} // namespace DodgeSpeed
