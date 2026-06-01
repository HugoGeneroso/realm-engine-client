#pragma once

#include "ZaclinDodgeTypes.h"

namespace ZaclinDodge::Debug {

void Render(const DebugSnapshot& snapshot, const Settings& settings, float camX, float camY, float angle, float zoom, float cx, float cy);

} // namespace ZaclinDodge::Debug