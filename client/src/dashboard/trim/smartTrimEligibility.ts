import type { ClientRole } from '../process/exaltClientRoles.js';
import type { ExaltProcessRow } from '../process/rotmgWindowsClientTune.js';

import type { SmartTrimSettings } from './smartTrimSettings.js';

const PARENT_IMG_L = 'rotmg exalt.exe';
const CHILD_IMG_L = 'rotmgexalt.exe';

function normKey(s: string | undefined): string {
  return String(s || '')
    .replace(/\u00a0/g, ' ')
    .trim()
    .toLowerCase();
}

const ROLE_TRIM_DEFAULT = {
  activeTrimEligible: false,
  backgroundTrimEligible: true,
  parkedTrimEligible: true,
} as const;

/**
 * PIDs to call `EmptyWorkingSet` on, after smart-trim rules (image filter, CPU cap, optional min WS).
 * Does **not** check system memory % — callers gate that separately.
 *
 * Role filtering requires `resolvePidRolesForProcesses()` from `exaltRealmClusters.ts` (async upstream).
 */
export function selectEmptyWorkingSetPids(
  ex: SmartTrimSettings['exalt'],
  processes: ExaltProcessRow[],
  opts?: { pidToRole?: Map<number, ClientRole> },
): number[] {
  const wantParent = ex.trimParentWs === true;
  const wantChild = ex.trimChildWs !== false;

  let cand = processes.filter((p) => {
    const k = normKey(p.imageName);
    if (wantParent && k === PARENT_IMG_L) return true;
    if (wantChild && k === CHILD_IMG_L) return true;
    return false;
  });

  if (cand.length === 0) {
    cand = [...processes];
  }

  const minWs =
    typeof ex.minWorkingSetBytesBeforeTrim === 'number' && ex.minWorkingSetBytesBeforeTrim > 0
      ? ex.minWorkingSetBytesBeforeTrim
      : 0;

  const maxCpu =
    typeof ex.maxCpuPercentForTrim === 'number' && Number.isFinite(ex.maxCpuPercentForTrim)
      ? Math.max(0, ex.maxCpuPercentForTrim)
      : 0;

  cand = cand.filter((p) => {
    const ws = Number(p.workingSetBytes) || 0;
    if (minWs > 0 && ws < minWs) return false;
    if (maxCpu > 0 && p.cpuPercent != null) {
      const c = Number(p.cpuPercent);
      if (Number.isFinite(c) && c > maxCpu) return false;
    }
    return true;
  });

  const seen = new Set<number>();
  let out: number[] = [];
  for (const p of cand) {
    const pid = Math.floor(Number(p.pid));
    if (!(pid > 0) || seen.has(pid)) continue;
    seen.add(pid);
    out.push(pid);
  }

  const polRaw = ex.trimRolePolicy;
  const pol = polRaw
    ? {
        activeTrimEligible: polRaw.activeTrimEligible ?? ROLE_TRIM_DEFAULT.activeTrimEligible,
        backgroundTrimEligible:
          polRaw.backgroundTrimEligible ?? ROLE_TRIM_DEFAULT.backgroundTrimEligible,
        parkedTrimEligible: polRaw.parkedTrimEligible ?? ROLE_TRIM_DEFAULT.parkedTrimEligible,
      }
    : null;

  if (pol && opts?.pidToRole && opts.pidToRole.size > 0) {
    out = out.filter((pid) => {
      const role = opts.pidToRole!.get(pid) ?? 'background';
      if (role === 'active') return pol.activeTrimEligible;
      if (role === 'parked') return pol.parkedTrimEligible;
      return pol.backgroundTrimEligible;
    });
  }

  return out;
}
