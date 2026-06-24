#pragma once

#include "keybinds.h"

class Settings {
public:
 KeyBinds::Config KeyBinds = {
 VK_TAB // toggle menu
 };

 bool ImGuiInitialized = false;
 bool bShowMenu = false;
 bool bEnableUnityLogs = true;

 // Developer diagnostics egress (MCP bridge). When on, DiagBridge mirrors live
 // game/IL2CPP state to %LOCALAPPDATA%\RealmEngine\*.json so the re-mcp server
 // (internal/tools/re-mcp) can runtime-test offset recovery and dodge behaviour.
 // Off by default — a normal user never writes these files; a developer flips it
 // on from the Test tab. Toggled at runtime, not compiled out.
 bool bEnableDiagBridge = false;
};

extern Settings settings;