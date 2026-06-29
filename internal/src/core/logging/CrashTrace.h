#pragma once
// CrashTrace — last-known site + ring buffer, flushed on hard crash (VEH).
// Output: %LOCALAPPDATA%\RotMG Exalt DLL Crash.log

#include <cstdint>

namespace CrashTrace {

void Install();
void Push(const char* msg);
void SetPresentStep(const char* step, uint32_t frame);

} // namespace CrashTrace
