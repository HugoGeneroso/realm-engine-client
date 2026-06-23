#pragma once

#include "ReppTypes.h"

// RE++ debug overlay (Z4). Draws tracked threats + predicted paths, sampled
// candidates with their safe/reject state, the intent + slide vectors, and the
// selected target. Pure renderer — takes a published snapshot + camera state.
namespace RePP { namespace Debug {

void Render(const DebugSnapshot& snapshot, const Settings& settings,
            float camX, float camY, float angle, float zoom, float cx, float cy);

} } // namespace RePP::Debug

