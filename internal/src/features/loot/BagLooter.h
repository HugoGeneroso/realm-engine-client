#pragma once
#include <cstdint>

// BagLooter — bag detection + auto-walk to the nearest loot bag.
//
// Per game-thread tick (throttled), scans WorldTAB::GetEntities() for loot
// bags whose object type matches an enabled tier, picks the nearest one
// within `maxWalkDistance`, and routes the dodge planner to it via
// DangerPlanner::SetExternalGoal. When the player is on top of the bag the
// goal is cleared and the planner returns to its previous behavior (lock-
// follow / idle / etc).
//
// Scope is movement only: this walks you onto the bag. Taking the items is
// the job of the external client's auto-loot plugin, not this module.
namespace BagLooter {

enum BagTier : int {
    Brown      = 0,   // 0x500 — common items
    Pink       = 1,   // 0x506 — basic stat pots, low-tier items
    Purple     = 2,   // 0x507 — better pots, eggs
    Cyan       = 3,   // 0x508 — consumables / stat pots
    Blue       = 4,   // 0x509 — UTs / rare drops
    White      = 5,   // 0x6be (Loot Bag 5 Boost) — UT-tier
    Soulbound  = 6,   // 0x503 — only your own drops
    TierCount_ = 7,
};

void Tick();
void Reset();

void SetEnabled(bool on);
bool IsEnabled();

void SetTierEnabled(BagTier t, bool on);
bool IsTierEnabled(BagTier t);

void  SetMaxWalkDistance(float tiles);   // 1..40, default 12
float GetMaxWalkDistance();

// Diagnostics — for the Movement-tab status row.
int32_t GetActiveBagId();        // 0 if no bag being pursued
float   GetActiveBagDistance();  // tiles from player; 0 if no active bag
const char* GetLastStatusTag();  // "off" / "no-player" / "hp-gated" / "no-bags" / "walking" / "arrived"

} // namespace BagLooter
