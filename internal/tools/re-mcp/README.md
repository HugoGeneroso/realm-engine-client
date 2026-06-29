# Realm Engine diagnostics MCP server

Lets an MCP client (Claude) **runtime-test the live game DLL** — read BootGate /
offset-recovery state, trigger IL2CPP queries on demand, and watch the dodge
engine's decisions — without a human reading the trace log.

```
 game.exe + version.dll  ──►  %LOCALAPPDATA%\RealmEngine\{diag,cmd,resp}.json  ──►  server.mjs (WSL)  ──►  Claude (MCP)
   DiagBridge.cpp                 (file egress, crosses WSL↔Windows)               re_* tools
```

The DLL reads live game memory / IL2CPP metadata natively; files are just the
egress, chosen because a WSL process reads `/mnt/c/...` directly (it cannot open a
Windows shared-memory object).

## File protocol (`%LOCALAPPDATA%\RealmEngine\`)

- **`diag.json`** — rewritten ~1 Hz (atomic). BootGate state, the dependency
  anchors (stale/healthy), the recovered projectile class, player stats, and a
  `dodge` section (status, target lock, threat/blocker counts).
- **`cmd.json`** — `{ "id": <ms>, "cmd": "...", "arg": "..." }`. The DLL polls
  this ~5×/s; a new `id` triggers one execution.
- **`resp.json`** — `{ "id": <same>, "ok": bool, "result": "..." }`.

## Tools

| Tool | What it does | Needs game |
|---|---|---|
| `re_status` | full `diag.json` snapshot (BootGate + anchors + player + dodge) | running |
| `re_run_recovery` | runs `AutoResolveByStructure` live, reports recovered class | running |
| `re_resolve_class` | `Resolver::FindClassLoose(name)` — exists? | running |
| `re_field_offset` | live `il2cpp_field_get_offset(class, field)` | running |
| `re_dodge_state` | dodge internals (status, lock, threats) for behaviour debugging | running + dodge on |

When the game isn't running, tools return a clear "diag.json not found / no
response" message instead of failing.

## Register it (you must approve this — an agent can't self-register an MCP server)

Add to a project `.mcp.json` (repo root) **or** run the CLI:

```jsonc
// .mcp.json
{ "mcpServers": {
    "realm-engine-diag": {
      "command": "node",
      "args": ["/home/jesse/realm-engine-client/internal/tools/re-mcp/server.mjs"]
    } } }
```
```bash
claude mcp add realm-engine-diag -- node /home/jesse/realm-engine-client/internal/tools/re-mcp/server.mjs
```

Then restart / reconnect the session so the server loads. Override the diag dir
with `RE_DIAG_DIR` if your Windows user isn't `Jesse`.

## Typical runtime test

1. Build + deploy `version.dll`, launch the game (login screen is enough — class
   metadata is live there).
2. Claude calls `re_status` → sees BootGate state + which anchors are stale.
3. Claude calls `re_run_recovery` → confirms it recovered the projectile class at
   the expected offset. That's the A1 fix, verified live.
