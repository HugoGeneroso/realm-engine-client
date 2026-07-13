#pragma once
// WorldGate — quiesce plugin writes/hooks across map/realm transitions.
//
// Root cause of the "return to Nexus" Unity crash: a write through a
// stale object pointer during the world-reload window (player object
// destroyed/recreated). The game only dies ~5s later (GC / integrity),
// so the corruption is seeded at the transition, not at crash time.
//
// Fix: detect the local-player pointer change (fires ONCE per transition),
// tear down every MinHook detour + writer on that edge, and keep them
// disabled until the new world has been STABLE for kSettleMs. Because
// every local-ptr change resets the settle timer, the gate only clears
// once the new map has actually settled.
#include <cstdint>

namespace WorldGate {

    // Call from GameState::Tick when the local player pointer changes
    // from one valid value to a different valid value (i.e. a new map).
    void OnLocalPtrChanged();

    // True exactly once per transition edge (single consumer, render thread).
    bool ConsumeWorldChange();

    // True while the new world is still settling (gate writers/hooks).
    bool IsSettling();

    // Tear down all hooks/writers. Called once per transition edge.
    void OnWorldChange();

} // namespace WorldGate
