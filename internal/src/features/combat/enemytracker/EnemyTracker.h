#pragma once

#include <cstdint>
#include <vector>

// Shared enemy snapshot (Present + game Update threads). Call Tick() (self-
// throttled ~125 Hz, mutex + SEH) then read via SnapshotCopy / Enumerate.
// Velocity fields (vx, vy) are tiles/ms, blended from MoVelocity + chord est.
namespace EnemyTracker {

struct Entry {
    int32_t id;
    int32_t objType;
    float   x, y;
    int32_t hp, maxHp;
    float   vx, vy;          // tiles/ms; 0 until first velocity sample
    bool    isInvulnerable;  // XML <Invincible/> flag
    bool    hasHealthBar;    // false for walls/destructibles (noHealthBar)
    void*   ptr;             // raw entity pointer (for direct field reads)
};

// Rebuilds the snapshot from the world dictionary. Self-throttled; redundant
// calls within 8 ms are cheap no-ops. Thread-safe (exclusive lock + SEH).
void Tick();

// Thread-safe copy of the last successful snapshot (shared lock).
std::vector<Entry> SnapshotCopy();

using Callback = void(*)(const Entry&, void* user);
void Enumerate(Callback cb, void* user);

// Object ID of the local player's world-dict entry, updated each Tick.
// More reliable than ProjectileTracking::GetLocalPlayerObjectId() which
// depends on WorldTAB having fired at least once.
int32_t GetLocalPlayerObjectId();

// Last rebuild stats (for diag.json / trace).
int GetLastPodCount();
int GetLastSnapshotCount();

} // namespace EnemyTracker
