#pragma once

#include "IpcBridge.h"
#include "Handshake.h"
#include "xorstr.h"
#include "main.h"
#include "settings.h"
#include "DbgFileLog.h"

#if __has_include("BuildSecrets.h")
#include "BuildSecrets.h"
#endif

#ifndef BUILD_PIPE_NAME
#define BUILD_PIPE_NAME "\\\\.\\pipe\\lfg-dev-bridge"
#endif

#include "LocalPlayer.h"
#include "GameState.h"
#include "RuntimeOffsets.h"
#include "AutoAim.h"
#include "ProjNoclip.h"
#include "FpsSetter.h"
#include "Noclip.h"
#include "GhostHit.h"
#include "SkinChanger.h"
#include "gui/tabs/TestTAB.h"
#include "gui/tabs/CameraTAB.h"
#include "gui/tabs/CombatTab/CombatTAB.h"
#include "DangerPlanner.h"
#include "XDodge.h"
#include "RolloutDodge.h"
#include "SpeedHack.h"
#include "Il2CppResolver.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <climits>
#include <limits>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <atomic>
#include <string>
#include <iostream>
