import { execFileSync } from 'child_process';
import { existsSync, readFileSync, unlinkSync, statSync, readdirSync } from 'fs';
import { join } from 'path';
import { tmpdir } from 'os';
import type net from 'net';
import { Logger } from '../util/Logger.js';
import type { ServerDirectory } from '../services/ServerDirectory.js';
import {
  getCredentialLaunchByLauncherPid,
  getCredentialLaunchByUnityPid,
  listCredentialLaunchRecords,
} from '../dashboard/process/credentialLaunchRegistry.js';

const TARGET_FILE = join(tmpdir(), 'rotmg_proxy_target.txt');
const TARGET_FILE_PREFIX = 'rotmg_proxy_target_';
/** Legacy shared file is only trusted if written within this window. */
const LEGACY_MAX_AGE_MS = 15_000;
/** Any per-PID DLL target file written within this window. */
const RECENT_DLL_MAX_AGE_MS = 120_000;
/** Credential launch server name fallback within this window. */
const LAUNCH_MAX_AGE_MS = 10 * 60_000;

/** @deprecated Use pickDefaultServerHost from ServerDirectory.ts */
export function pickDefaultServerIp(servers: Record<string, string>): string {
  return servers.USWest
    || servers.USEast
    || servers.EUWest
    || Object.values(servers)[0]
    || 'uswest3.realmofthemadgod.com';
}

/**
 * Resolves the real game-server host for a new client TCP connection.
 * Priority: per-PID DLL file → recent DLL file → credential-launch serverName → fresh legacy → default.
 */
export class LaunchTargetResolver {
  constructor(private readonly directory: ServerDirectory) {}

  get defaultHost(): string {
    return this.directory.defaultHost;
  }

  resolveForSocket(socket: net.Socket): string {
    const fromPid = this.readFromDllPidFiles(socket);
    if (fromPid) return fromPid;

    // Credential launch (explicit server pick) beats a stale temp file from another session.
    const fromLaunch = this.readFromCredentialLaunch(socket);
    if (fromLaunch) return fromLaunch;

    const fromRecent = this.readNewestRecentTargetFile();
    if (fromRecent) return fromRecent;

    const fromLegacy = this.readLegacyIfFresh();
    if (fromLegacy) return fromLegacy;

    const fallback = this.directory.defaultHost;
    Logger.warn('LaunchTarget', `No DLL/launch target found — using default ${fallback}`);
    return fallback;
  }

  cleanStaleFiles(): void {
    try {
      if (existsSync(TARGET_FILE)) unlinkSync(TARGET_FILE);
    } catch {}
    try {
      const tmp = tmpdir();
      for (const f of readdirSync(tmp)) {
        if (f.startsWith(TARGET_FILE_PREFIX) && f.endsWith('.txt')) {
          try { unlinkSync(join(tmp, f)); } catch {}
        }
      }
    } catch {}
  }

  private readFromDllPidFiles(socket: net.Socket): string {
    for (const pid of this.resolveOwningProcessIds(socket)) {
      const direct = this.readTargetFile(join(tmpdir(), `${TARGET_FILE_PREFIX}${pid}.txt`), true);
      if (direct) {
        Logger.log('LaunchTarget', `DLL target for PID ${pid}: ${direct}`);
        return direct;
      }

      const byLauncher = getCredentialLaunchByLauncherPid(pid);
      if (byLauncher?.pidUnity) {
        const unity = this.readTargetFile(
          join(tmpdir(), `${TARGET_FILE_PREFIX}${byLauncher.pidUnity}.txt`),
          true,
        );
        if (unity) {
          Logger.log('LaunchTarget', `DLL target for Unity PID ${byLauncher.pidUnity} (launcher ${pid}): ${unity}`);
          return unity;
        }
      }

      const byUnity = getCredentialLaunchByUnityPid(pid);
      if (byUnity?.pidLauncher) {
        const launcher = this.readTargetFile(
          join(tmpdir(), `${TARGET_FILE_PREFIX}${byUnity.pidLauncher}.txt`),
          true,
        );
        if (launcher) {
          Logger.log('LaunchTarget', `DLL target for launcher PID ${byUnity.pidLauncher} (Unity ${pid}): ${launcher}`);
          return launcher;
        }
      }
    }
    return '';
  }

  /** Newest rotmg_proxy_target*.txt in %TEMP% (handles Unity vs launcher PID mismatch). */
  private readNewestRecentTargetFile(): string {
    const candidates: Array<{ path: string; mtime: number }> = [];
    const now = Date.now();

    const consider = (path: string) => {
      try {
        if (!existsSync(path)) return;
        const mtime = statSync(path).mtimeMs;
        if (now - mtime > RECENT_DLL_MAX_AGE_MS) return;
        candidates.push({ path, mtime });
      } catch {}
    };

    consider(TARGET_FILE);
    try {
      for (const f of readdirSync(tmpdir())) {
        if (f.startsWith(TARGET_FILE_PREFIX) && f.endsWith('.txt')) {
          consider(join(tmpdir(), f));
        }
      }
    } catch {}

    candidates.sort((a, b) => b.mtime - a.mtime);
    for (const { path } of candidates) {
      const ip = this.readTargetFile(path, false);
      if (ip) {
        Logger.log('LaunchTarget', `Using recent DLL target (${path}): ${ip}`);
        return ip;
      }
    }
    return '';
  }

  private readFromCredentialLaunch(socket: net.Socket): string {
    for (const pid of this.resolveOwningProcessIds(socket)) {
      const host = this.hostFromLaunchRecord(
        getCredentialLaunchByUnityPid(pid) || getCredentialLaunchByLauncherPid(pid),
        pid,
      );
      if (host) return host;
    }

    const recent = listCredentialLaunchRecords().find(
      (r) => Date.now() - r.launchedAtMs <= LAUNCH_MAX_AGE_MS && r.serverName,
    );
    if (recent) {
      const host = this.hostFromLaunchRecord(recent, recent.pidUnity ?? recent.pidLauncher);
      if (host) return host;
    }
    return '';
  }

  private hostFromLaunchRecord(
    rec: { serverName: string | null; launchedAtMs: number } | undefined,
    pid: number,
  ): string {
    if (!rec?.serverName) return '';
    const host = this.directory.getHost(rec.serverName);
    if (!host) return '';
    Logger.log('LaunchTarget', `Using launch server ${rec.serverName} (${host}) for PID ${pid}`);
    return host;
  }

  private readLegacyIfFresh(): string {
    try {
      if (!existsSync(TARGET_FILE)) return '';
      const age = Date.now() - statSync(TARGET_FILE).mtimeMs;
      if (age > LEGACY_MAX_AGE_MS) {
        Logger.warn(
          'LaunchTarget',
          `Ignoring stale legacy target file (${Math.round(age / 1000)}s old — was ${readFileSync(TARGET_FILE, 'utf8').trim()})`,
        );
        return '';
      }
      const ip = this.readTargetFile(TARGET_FILE);
      if (ip) Logger.log('LaunchTarget', `Using fresh legacy DLL target: ${ip}`);
      return ip;
    } catch {
      return '';
    }
  }

  private readTargetFile(filePath: string, deleteAfter = false): string {
    try {
      if (!existsSync(filePath)) return '';
      const value = readFileSync(filePath, 'utf8').trim();
      if (!value || value === '127.0.0.1') return '';
      const isIp = /^\d+\.\d+\.\d+\.\d+$/.test(value);
      const isHost = /^[a-z0-9.-]+\.[a-z]{2,}$/i.test(value);
      if (!isIp && !isHost) return '';
      if (deleteAfter) {
        try { unlinkSync(filePath); } catch {}
      }
      return value;
    } catch {
      return '';
    }
  }

  private resolveOwningProcessIds(socket: net.Socket): number[] {
    const pids = new Set<number>();
    const clientPort = socket.remotePort;
    if (!clientPort || process.platform !== 'win32') return [];

    const queries = [
      `(Get-NetTCPConnection -LocalPort ${clientPort} -RemotePort 2050 -State Established -ErrorAction SilentlyContinue | Select-Object -First 1).OwningProcess`,
      `(Get-NetTCPConnection -RemoteAddress 127.0.0.1 -RemotePort 2050 -State Established -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -eq ${clientPort} } | Select-Object -First 1).OwningProcess`,
    ];

    for (const query of queries) {
      try {
        const output = execFileSync(
          'powershell.exe',
          ['-NonInteractive', '-NoProfile', '-Command', query],
          { encoding: 'utf8', timeout: 2500, windowsHide: true } as any,
        ).trim();
        const pid = parseInt(output, 10);
        if (Number.isFinite(pid) && pid > 0) pids.add(pid);
      } catch {
        // try next query
      }
    }

    return [...pids];
  }
}
