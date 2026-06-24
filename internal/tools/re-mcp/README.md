# Realm Engine diagnostics MCP server (RotMG MCP)

An [MCP](https://modelcontextprotocol.io) server that lets an MCP client (Claude)
**runtime-test the live game DLL** — read BootGate / offset-recovery state, trigger
IL2CPP queries on demand, and watch the dodge engine's decisions — without a human
reading the trace log.

```
 game.exe + version.dll  ──►  %LOCALAPPDATA%\RealmEngine\{diag,cmd,resp}.json  ──►  server.mjs (WSL)  ──►  Claude (MCP)
   DiagBridge.cpp                 (file egress, crosses WSL↔Windows)               re_* tools
```

The DLL reads live game memory / IL2CPP metadata natively; files are just the
egress, chosen because a WSL process reads `/mnt/c/...` directly (it cannot open a
Windows shared-memory object).

> **Two opt-ins, both off by default.** Nothing here runs unless *you* turn it on:
> 1. **In the game** — flip *Test tab → DIAGNOSTICS BRIDGE (MCP) → Enable diagnostics
>    egress*. A normal user never writes these files; the diag code is dormant until
>    a developer toggles it. (Runtime toggle — no special build required.)
> 2. **In your MCP client** — approve the bundled `.mcp.json` (below). Project MCP
>    servers are per-user approved, so a clone never auto-runs code.

## File protocol (`%LOCALAPPDATA%\RealmEngine\`)

- **`diag.json`** — rewritten ~1 Hz (atomic) while the bridge is on. BootGate state,
  the dependency anchors (stale/healthy), the recovered projectile class, player
  stats, and a `dodge` section (status, target lock, threat/blocker counts).
- **`cmd.json`** — `{ "id": <ms>, "cmd": "...", "arg": "..." }`. The DLL polls this
  ~5×/s; a new `id` triggers one execution.
- **`resp.json`** — `{ "id": <same>, "ok": bool, "result": "..." }`.

Toggling the bridge **off** deletes `diag/cmd/resp.json` so a stale snapshot is never
read as if the game were still live.

## Tools

| Tool | What it does | Needs game |
|---|---|---|
| `re_status` | full `diag.json` snapshot (BootGate + anchors + player + dodge) | running |
| `re_run_recovery` | runs `AutoResolveByStructure` live, reports recovered class | running |
| `re_resolve_class` | `Resolver::FindClassLoose(name)` — exists? | running |
| `re_field_offset` | live `il2cpp_field_get_offset(class, field)` | running |
| `re_dodge_state` | dodge internals (status, lock, threats) for behaviour debugging | running + dodge on |
| `re_dump_report` | full live offset audit (Match / Shifted / FieldRenamed / …) | running |
| `re_class_methods` | live method RVAs for a class | running |
| `re_probe_aoe` | AoE throwable field-offset self-heal status | running + cast seen |

When the game isn't running (or the bridge is off), tools return a clear
"diag.json not found / no response" message instead of failing.

## Use it

1. **Enable the bridge in-game.** Build/deploy `version.dll`, launch the game, open
   the menu (`Tab`) → **Test** tab → tick **Enable diagnostics egress (MCP bridge)**.
   The login screen is enough — class metadata is live there.

2. **Register the MCP server.** This repo ships a project-scoped
   [`.mcp.json`](../../../.mcp.json) at the repo root — open the project in Claude
   Code and approve `realm-engine-diag` when prompted (project servers are per-user
   approved; an agent can't self-register one):

   ```jsonc
   // .mcp.json (repo root) — already committed
   { "mcpServers": {
       "realm-engine-diag": {
         "type": "stdio",
         "command": "node",
         "args": ["${CLAUDE_PROJECT_DIR:-.}/internal/tools/re-mcp/server.mjs"]
       } } }
   ```

   `${CLAUDE_PROJECT_DIR:-.}` resolves to the repo root for any clone location, so no
   absolute path is baked in. Prefer the CLI instead? From the repo root:

   ```bash
   claude mcp add realm-engine-diag -- node "$PWD/internal/tools/re-mcp/server.mjs"
   ```

3. **Override the diag dir** with `RE_DIAG_DIR` if your Windows user isn't `Jesse`
   (the server otherwise auto-detects `%LOCALAPPDATA%` via `cmd.exe`).

The server is **zero-dependency** Node (no `npm install`) and speaks MCP stdio.

## Typical runtime test

1. Bridge on, game at the login screen.
2. Claude calls `re_status` → sees BootGate state + which anchors are stale.
3. Claude calls `re_run_recovery` → confirms it recovered the projectile class at the
   expected offset. That's the fix, verified live.
