import chokidar from 'chokidar';
import { createHash } from 'crypto';
import { copyFileSync, existsSync, readFileSync } from 'fs';
import { execFileSync } from 'child_process';
import { Logger } from './Logger.js';

function sha256File(path: string): string | null {
  try {
    return createHash('sha256').update(readFileSync(path)).digest('hex');
  } catch {
    return null;
  }
}

function killRotmgProcesses(): void {
  for (const image of ['RotMGExalt.exe', 'RotMG Exalt.exe']) {
    try {
      execFileSync('taskkill', ['/IM', image, '/F'], { windowsHide: true, stdio: 'ignore' });
    } catch {
      // not running
    }
  }
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

export type VersionDllHotReloadOptions = {
  assetsDll: string;
  gameDll: string;
  /** Called after a successful deploy (game was killed so the new DLL can load on next launch). */
  relaunchGame?: () => { ok: boolean; error?: string };
  onStatus?: (msg: { type: 'dllHotReload'; status: string; detail?: string }) => void;
};

/**
 * Watches client/assets/version.dll and redeploys to Production when MSBuild
 * finishes. Kills the running Exalt client so Windows releases the file lock,
 * then optionally relaunches. Enabled in dev unless REALM_ENGINE_HOT_DLL=0.
 */
export function startVersionDllHotReload(opts: VersionDllHotReloadOptions): void {
  if (process.env.REALM_ENGINE_HOT_DLL === '0') return;
  if (!existsSync(opts.assetsDll)) {
    Logger.warn('HotDll', `assets DLL not found — hot reload off (${opts.assetsDll})`);
    return;
  }

  let lastDeployedHash = sha256File(opts.gameDll);
  let busy = false;

  const notify = (status: string, detail?: string) => {
    Logger.log('HotDll', detail ? `${status}: ${detail}` : status);
    opts.onStatus?.({ type: 'dllHotReload', status, detail });
  };

  const deploy = async () => {
    if (busy) return;
    if (!existsSync(opts.assetsDll)) return;

    const srcHash = sha256File(opts.assetsDll);
    if (!srcHash || srcHash === lastDeployedHash) return;

    busy = true;
    notify('building', `new DLL detected (${srcHash.slice(0, 12)}…)`);

    try {
      for (let attempt = 0; attempt < 3; attempt++) {
        try {
          copyFileSync(opts.assetsDll, opts.gameDll);
          lastDeployedHash = srcHash;
          notify('deployed', opts.gameDll);
          break;
        } catch (err) {
          if (attempt >= 2) throw err;
          notify('unlocking', 'game has DLL locked — closing Exalt');
          killRotmgProcesses();
          await sleep(1200);
        }
      }

      killRotmgProcesses();
      await sleep(800);

      if (opts.relaunchGame) {
        const result = opts.relaunchGame();
        if (result.ok) {
          notify('relaunched', 'RotMG Exalt restarted with new version.dll');
        } else {
          notify('deployed', result.error || 'deploy OK — launch game from dashboard');
        }
      } else {
        notify('deployed', 'launch game from dashboard to load new DLL');
      }
    } catch (err) {
      notify('failed', (err as Error).message);
    } finally {
      busy = false;
    }
  };

  chokidar
    .watch(opts.assetsDll, {
      ignoreInitial: true,
      awaitWriteFinish: { stabilityThreshold: 900, pollInterval: 200 },
    })
    .on('add', () => void deploy())
    .on('change', () => void deploy());

  notify('watching', opts.assetsDll);
}
