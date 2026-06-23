#!/usr/bin/env node
// Realm Engine diagnostics MCP server (zero dependencies).
//
// Bridges the live game DLL to an MCP client (Claude). The injected DLL
// (DiagBridge.cpp) mirrors its state to %LOCALAPPDATA%\RealmEngine\diag.json and
// services commands via cmd.json/resp.json. This server runs in WSL, reads those
// files over /mnt/c, and exposes them as MCP tools so an agent can RUNTIME-test
// BootGate + offset recovery while the game runs.
//
// Transport: MCP stdio = newline-delimited JSON-RPC 2.0 (no embedded newlines).
//
// Diag dir resolution (first that works):
//   1. $RE_DIAG_DIR
//   2. `cmd.exe /c echo %LOCALAPPDATA%` -> wslpath
//   3. /mnt/c/Users/Jesse/AppData/Local/RealmEngine  (fallback)

import { readFileSync, writeFileSync, existsSync, renameSync } from 'node:fs';
import { execSync } from 'node:child_process';
import { join } from 'node:path';
import readline from 'node:readline';

// ── locate the diag directory ────────────────────────────────────────────────
function resolveDiagDir() {
  if (process.env.RE_DIAG_DIR) return process.env.RE_DIAG_DIR;
  try {
    const winLocal = execSync('cmd.exe /c echo %LOCALAPPDATA%', { encoding: 'utf8' }).trim();
    if (winLocal && winLocal !== '%LOCALAPPDATA%') {
      const unix = execSync(`wslpath -u ${JSON.stringify(winLocal)}`, { encoding: 'utf8' }).trim();
      return join(unix, 'RealmEngine');
    }
  } catch { /* fall through */ }
  return '/mnt/c/Users/Jesse/AppData/Local/RealmEngine';
}
const DIAG_DIR  = resolveDiagDir();
const DIAG_FILE   = join(DIAG_DIR, 'diag.json');
const CMD_FILE    = join(DIAG_DIR, 'cmd.json');
const RESP_FILE   = join(DIAG_DIR, 'resp.json');
const REPORT_FILE = join(DIAG_DIR, 'report.json');
const METHODS_FILE= join(DIAG_DIR, 'methods.json');

function readJson(path, label) {
  if (!existsSync(path)) return { error: `${label} not found at ${path} — is the game running?` };
  try { return JSON.parse(readFileSync(path, 'utf8')); }
  catch (e) { return { error: `${label} unreadable/partial: ${e.message}` }; }
}

function readDiag() {
  if (!existsSync(DIAG_FILE)) return { error: `diag.json not found at ${DIAG_FILE} — is the game running with the DLL injected?` };
  try { return JSON.parse(readFileSync(DIAG_FILE, 'utf8')); }
  catch (e) { return { error: `diag.json unreadable/partial: ${e.message}` }; }
}

// Issue a command to the DLL and wait for the matching response (v2 DLL handler).
async function runCommand(cmd, arg = '', timeoutMs = 3000) {
  const id = Date.now();
  const tmp = CMD_FILE + '.tmp';
  writeFileSync(tmp, JSON.stringify({ id, cmd, arg }));
  renameSync(tmp, CMD_FILE);            // atomic for the DLL poller
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (existsSync(RESP_FILE)) {
      try {
        const r = JSON.parse(readFileSync(RESP_FILE, 'utf8'));
        if (r && r.id === id) return r;
      } catch { /* partial write; retry */ }
    }
    await new Promise(r => setTimeout(r, 50));
  }
  return { id, ok: false, error: 'no response from DLL (is it running with the v2 command handler?)' };
}

// ── tool definitions ─────────────────────────────────────────────────────────
const TOOLS = [
  { name: 're_status', description: 'Live BootGate state, the 10 dependency anchors (stale/healthy), the recovered projectile class, and player stats — read from the running game DLL. Use to runtime-verify offset recovery.', inputSchema: { type: 'object', properties: {} } },
  { name: 're_run_recovery', description: 'Trigger the DLL to run AutoResolveByStructure now and report what it recovered (projectile class + offset). Needs the game running.', inputSchema: { type: 'object', properties: {} } },
  { name: 're_resolve_class', description: 'Ask the live DLL to resolve an IL2CPP class by name and return whether it exists.', inputSchema: { type: 'object', properties: { name: { type: 'string', description: 'class name (obfuscated or real)' } }, required: ['name'] } },
  { name: 're_field_offset', description: 'Ask the live DLL for a field offset on a class (live il2cpp_field_get_offset).', inputSchema: { type: 'object', properties: { className: { type: 'string' }, fieldName: { type: 'string' } }, required: ['className', 'fieldName'] } },
  { name: 're_dodge_state', description: 'Live dodge engine internals (player pos, target lock, last move target, threat samples, last decision) for debugging dodge behaviour. [v3]', inputSchema: { type: 'object', properties: {} } },
  { name: 're_dump_report', description: 'Full live offset audit: every registry entry with its committed fallback, the LIVE resolved offset, and a state (Match / Shifted=stale-fallback / FieldRenamed / ClassGaveUp / Suspect). The daily-scan source of truth — Shifted rows are stale fallbacks to patch.', inputSchema: { type: 'object', properties: {} } },
  { name: 're_class_methods', description: 'Live method RVAs for a class (name + RVA = methodPointer − GameAssembly base). The always-current "method offsets".', inputSchema: { type: 'object', properties: { className: { type: 'string' } }, required: ['className'] } },
  { name: 're_probe_aoe', description: 'AoE throwable field-offset self-heal status: the KOBMINBDOBD hook matches the true origin/dest params against instance fields to recover the renamed offsets. resolved=false until one throwable is seen — trigger a Medusa cast in the Godlands.', inputSchema: { type: 'object', properties: {} } },
];

function asText(obj) { return { content: [{ type: 'text', text: JSON.stringify(obj, null, 2) }] }; }

async function callTool(name, args) {
  switch (name) {
    case 're_status':       return asText(readDiag());
    case 're_run_recovery': return asText(await runCommand('run_recovery'));
    case 're_resolve_class':return asText(await runCommand('resolve_class', String(args?.name ?? '')));
    case 're_field_offset': return asText(await runCommand('field_offset', `${args?.className ?? ''}.${args?.fieldName ?? ''}`));
    case 're_dodge_state':  { const d = readDiag(); return asText(d.dodge ?? { note: 'no dodge section yet (v3)', diag: d }); }
    case 're_dump_report':  { const r = await runCommand('dump_report', '', 6000); return r?.ok ? asText(readJson(REPORT_FILE, 'report.json')) : asText(r); }
    case 're_class_methods':{ const r = await runCommand('class_methods', String(args?.className ?? ''), 6000); return r?.ok ? asText(readJson(METHODS_FILE, 'methods.json')) : asText(r); }
    case 're_probe_aoe':    return asText(await runCommand('aoe_probe'));
    default: return { content: [{ type: 'text', text: `unknown tool ${name}` }], isError: true };
  }
}

// ── JSON-RPC over stdio ──────────────────────────────────────────────────────
function send(msg) { process.stdout.write(JSON.stringify(msg) + '\n'); }

const rl = readline.createInterface({ input: process.stdin });
rl.on('line', async (line) => {
  line = line.trim();
  if (!line) return;
  let req;
  try { req = JSON.parse(line); } catch { return; }
  const { id, method, params } = req;
  try {
    if (method === 'initialize') {
      send({ jsonrpc: '2.0', id, result: {
        protocolVersion: params?.protocolVersion || '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: 'realm-engine-diag', version: '0.1.0' },
      }});
    } else if (method === 'notifications/initialized') {
      // no response for notifications
    } else if (method === 'tools/list') {
      send({ jsonrpc: '2.0', id, result: { tools: TOOLS } });
    } else if (method === 'tools/call') {
      const result = await callTool(params?.name, params?.arguments || {});
      send({ jsonrpc: '2.0', id, result });
    } else if (method === 'ping') {
      send({ jsonrpc: '2.0', id, result: {} });
    } else if (id !== undefined) {
      send({ jsonrpc: '2.0', id, error: { code: -32601, message: `method not found: ${method}` } });
    }
  } catch (e) {
    if (id !== undefined) send({ jsonrpc: '2.0', id, error: { code: -32603, message: String(e?.message || e) } });
  }
});

process.stderr.write(`[re-mcp] up. diag dir = ${DIAG_DIR}\n`);
