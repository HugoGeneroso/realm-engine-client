import { Logger } from '../../util/Logger.js';
import {
  emptyWorkingSetAllExalt,
  emptyWorkingSetForPids,
  getSystemMemoryStatus,
  listExaltProcesses,
  tuningSupported,
} from '../process/rotmgWindowsClientTune.js';
import { selectEmptyWorkingSetPids } from './smartTrimEligibility.js';
import { resolvePidRolesForProcesses } from '../process/exaltRealmClusters.js';
import { loadSmartTrimSettings } from './smartTrimSettings.js';

const MASTER_TICK_MS = 12_000;

export type TrimProxySmartOptions = {
  trimPackets: boolean;
  trimPacketLab: boolean;
  trimWorldSnapshot: boolean;
  runGcHint: boolean;
};

export type SmartTrimDeps = {
  /** Current Node RSS (bytes). */
  getRss: () => number;
  /** Packets/sec from PacketInspector.getRate(). */
  getPacketRate: () => number;
  /** Apply buffer clears + optional `gc()`. */
  trimProxyMemory: (o: TrimProxySmartOptions) => void;
};

let timer: ReturnType<typeof setInterval> | null = null;

/** Prevents overlapping PowerShell / trim work if a tick stalls. */
let smartTrimInFlight = false;

let lastProxyEval = 0;
let lastProxyTrim = 0;
let lastExaltEval = 0;
let lastExaltTrim = 0;

async function smartTrimTick(deps: SmartTrimDeps): Promise<void> {
  const now = Date.now();
  const s = loadSmartTrimSettings();

  if (s.proxy.enabled && now - lastProxyEval >= s.proxy.checkIntervalMs) {
    lastProxyEval = now;
    const rss = deps.getRss();
    const rate = deps.getPacketRate();
    const rssHit = s.proxy.rssBytesThreshold > 0 && rss >= s.proxy.rssBytesThreshold;
    const rateHit =
      s.proxy.packetRateThreshold > 0 && rate >= s.proxy.packetRateThreshold;
    const shouldTrim = rssHit || rateHit;

    if (shouldTrim && now - lastProxyTrim >= s.proxy.minTrimIntervalMs) {
      try {
        deps.trimProxyMemory({
          trimPackets: s.proxy.trimPackets,
          trimPacketLab: s.proxy.trimPacketLab,
          trimWorldSnapshot: s.proxy.trimWorldSnapshot,
          runGcHint: s.proxy.runGcHint,
        });
        lastProxyTrim = Date.now();
        Logger.log(
          'smartTrim',
          `Proxy trim (rss=${Math.round(rss / 1048576)}MB rate=${rate}/s)`,
        );
      } catch (e) {
        Logger.warn('smartTrim', String((e as Error).message || e));
      }
    }
  }

  if (!s.exalt.enabled) return;

  if (now - lastExaltEval >= s.exalt.checkIntervalMs) {
    lastExaltEval = now;
    const tuned = await tuningSupported();
    if (!tuned.ok) return;

    const ex = s.exalt;
    if (ex.requireMemoryLoadPercent > 0) {
      const mem = await getSystemMemoryStatus();
      if (!mem || mem.memoryLoadPercent < ex.requireMemoryLoadPercent) {
        return;
      }
    }

    const list = await listExaltProcesses();
    const processes = list.processes || [];
    if (!processes.length) return;

    let maxWs = 0;
    for (const p of processes) {
      const ws = Number(p.workingSetBytes) || 0;
      if (ws > maxWs) maxWs = ws;
    }

    const thresh = ex.workingSetBytesPerProcessThreshold;
    const sizeHit = thresh > 0 && maxWs >= thresh;
    const periodicHit = ex.periodicTrim === true;

    if (
      (sizeHit || periodicHit) &&
      now - lastExaltTrim >= ex.minTrimIntervalMs
    ) {
      try {
        const pidToRole = await resolvePidRolesForProcesses(processes);
        const pids = selectEmptyWorkingSetPids(ex, processes, {
          pidToRole,
        });
        if (pids.length === 0) return;
        const r = await emptyWorkingSetForPids(pids);
        if (r.ok) {
          lastExaltTrim = Date.now();
          Logger.log(
            'smartTrim',
            `Exalt EmptyWorkingSet applied=${r.applied} pid(s) (maxWs=${Math.round(maxWs / 1048576)}MB)`,
          );
        }
      } catch (e) {
        Logger.warn('smartTrim', String((e as Error).message || e));
      }
    }
  }
}

export function attachSmartTrimScheduler(deps: SmartTrimDeps): void {
  if (timer != null) return;
  timer = setInterval(() => {
    if (smartTrimInFlight) return;
    smartTrimInFlight = true;
    smartTrimTick(deps)
      .catch((err) => Logger.warn('smartTrim', String((err as Error).message || err)))
      .finally(() => {
        smartTrimInFlight = false;
      });
  }, MASTER_TICK_MS);
}

export function stopSmartTrimScheduler(): void {
  if (timer != null) {
    clearInterval(timer as ReturnType<typeof setInterval>);
    timer = null;
  }
  lastProxyEval = 0;
  lastProxyTrim = 0;
  lastExaltEval = 0;
  lastExaltTrim = 0;
}

export function reloadSmartTrimTimerState(): void {
  /** User changed settings mid-run — reset evaluation windows so changes apply cleanly. */
  lastProxyEval = 0;
  lastExaltEval = 0;
}

/** Same logic as scheduler; `{ manual: true }` (Trim now) skips memory + max-CPU gates and works when automation is off (all Exalt PIDs). */
export async function trimExaltWorkingSetsFromDiskSettings(options?: {
  manual?: boolean;
}): Promise<{
  ok: boolean;
  applied: number;
  error?: string;
  skipped?: string;
}> {
  const tuned = await tuningSupported();
  if (!tuned.ok)
    return { ok: false, applied: 0, error: tuned.reason, skipped: 'unsupported' };

  const s = loadSmartTrimSettings();
  const manual = options?.manual === true;
  const ex = s.exalt;

  if (!manual && !ex.enabled) return { ok: true, applied: 0, skipped: 'disabled' };

  if (!manual && ex.requireMemoryLoadPercent > 0) {
    const mem = await getSystemMemoryStatus();
    if (!mem || mem.memoryLoadPercent < ex.requireMemoryLoadPercent) {
      return {
        ok: true,
        applied: 0,
        skipped: 'memory_below_threshold',
      };
    }
  }

  if (!ex.enabled && manual) {
    const { processes } = await listExaltProcesses();
    if (!processes.length) return { ok: true, applied: 0, skipped: 'no_processes' };
    const pidToRoleEarly = await resolvePidRolesForProcesses(processes);
    const exSweep = {
      ...ex,
      trimChildWs: true,
      trimParentWs: true,
      maxCpuPercentForTrim: 0,
      minWorkingSetBytesBeforeTrim: 0,
      workingSetBytesPerProcessThreshold: 0,
    };
    const pidsEarly = selectEmptyWorkingSetPids(exSweep, processes, {
      pidToRole: pidToRoleEarly,
    });
    if (pidsEarly.length === 0) {
      return { ok: true, applied: 0, skipped: 'no_matching_pids' };
    }
    return emptyWorkingSetForPids(pidsEarly);
  }

  const { processes } = await listExaltProcesses();
  if (!processes.length) return { ok: true, applied: 0, skipped: 'no_processes' };

  const exForPid = manual ? { ...ex, maxCpuPercentForTrim: 0 } : ex;

  const pidToRole = await resolvePidRolesForProcesses(processes);
  const pids = selectEmptyWorkingSetPids(exForPid, processes, { pidToRole });
  if (pids.length === 0) {
    return { ok: true, applied: 0, skipped: 'no_matching_pids' };
  }

  return emptyWorkingSetForPids(pids);
}
