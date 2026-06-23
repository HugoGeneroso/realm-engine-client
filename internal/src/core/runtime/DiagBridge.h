#pragma once

// DiagBridge — runtime diagnostics egress for live testing.
//
// The DLL runs inside the Windows game and reads live game memory / IL2CPP
// metadata natively. DiagBridge mirrors a snapshot of that state out to a file
// the host side can read, so an external reader, an MCP server, or the build loop
// can RUNTIME-test BootGate + offset recovery without a human watching the log.
//
// Transport = files under %LOCALAPPDATA%\RealmEngine\ (chosen because it crosses
// the WSL<->Windows boundary trivially — a WSL process reads /mnt/c/.../ directly,
// unlike a Windows shared-memory object):
//   diag.json  — state snapshot, rewritten ~1 Hz (atomic: tmp + rename) [v1]
//   cmd.json   — a request the DLL polls and executes                   [v2]
//   resp.json  — the DLL's response to the last command                 [v2]
//   diag.json also carries dodge internals for live behaviour debugging  [v3]
namespace DiagBridge {

    // Call once per frame from dPresent (self-throttles to ~1 Hz for the snapshot
    // and polls for commands). Cheap; safe on the render thread.
    void Tick();

} // namespace DiagBridge
