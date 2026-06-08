import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'fs';
import { join } from 'path';

import { getRealmengineDocumentsDir } from '../../util/rotmgAssetExtractor.js';

const FILE = 'smart-trim-settings.json';

export const SMART_TRIM_VERSION = 1 as const;

/** Respect multibox “active / fg” vs parked when choosing EmptyWorkingSet PIDs. */
export type SmartTrimRolePolicy = {
  activeTrimEligible: boolean;
  backgroundTrimEligible: boolean;
  parkedTrimEligible: boolean;
};

/** Automated memory hygiene for the Node proxy and RotMG Exalt.exe (Windows working-set trim). */
export type SmartTrimSettings = {
  version: typeof SMART_TRIM_VERSION;
  proxy: {
    enabled: boolean;
    /** How often we evaluate RSS / packet rate (ms). */
    checkIntervalMs: number;
    /** When non-zero and RSS exceeds this (bytes), trigger a trim. */
    rssBytesThreshold: number;
    /** Optional: when PacketInspector packets/sec exceeds this (0 = ignore). */
    packetRateThreshold: number;
    /** Never trim more often than this (ms). */
    minTrimIntervalMs: number;
    trimPackets: boolean;
    trimPacketLab: boolean;
    trimWorldSnapshot: boolean;
    runGcHint: boolean;
  };
  exalt: {
    enabled: boolean;
    checkIntervalMs: number;
    /** When any RotMG Exalt.exe working set exceeds this (bytes). 0 = never trigger by size (use periodic only). */
    workingSetBytesPerProcessThreshold: number;
    /** If true, trim Exalt working sets on every qualifying check interval even below threshold (gentle upkeep). */
    periodicTrim: boolean;
    minTrimIntervalMs: number;
    /**
     * If &gt; 0, trims only while system physical memory load is at least this % (Win32_OperatingSystem).
     * 0 = don't require OS memory pressure.
     */
    requireMemoryLoadPercent: number;
    /**
     * If &gt; 0, skip trimming a process whose sampled CPU% is above this (reduces trim during active play).
     */
    maxCpuPercentForTrim: number;
    /**
     * Don't trim a process unless its working set is at least this many bytes. 0 = no extra floor.
     */
    minWorkingSetBytesBeforeTrim: number;
    /** Include launcher `RotMG Exalt.exe` in EmptyWorkingSet (usually false). */
    trimParentWs: boolean;
    /** Include Unity child `RotMGExalt.exe` (usually true). */
    trimChildWs: boolean;
    /** Filters `EmptyWorkingSet` targets by FG/parked/active role — see `smartTrimEligibility`. */
    trimRolePolicy?: SmartTrimRolePolicy;
  };
};

export function defaultSmartTrimSettings(): SmartTrimSettings {
  return {
    version: SMART_TRIM_VERSION,
    proxy: {
      enabled: false,
      checkIntervalMs: 20_000,
      rssBytesThreshold: 380 * 1024 * 1024,
      packetRateThreshold: 450,
      minTrimIntervalMs: 55_000,
      trimPackets: true,
      trimPacketLab: true,
      trimWorldSnapshot: false,
      runGcHint: true,
    },
    exalt: {
      enabled: false,
      checkIntervalMs: 35_000,
      workingSetBytesPerProcessThreshold: Math.round(2.25 * 1024 * 1024 * 1024),
      periodicTrim: false,
      minTrimIntervalMs: 180_000,
      requireMemoryLoadPercent: 85,
      maxCpuPercentForTrim: 10,
      minWorkingSetBytesBeforeTrim: 0,
      trimParentWs: false,
      trimChildWs: true,
      trimRolePolicy: {
        activeTrimEligible: false,
        backgroundTrimEligible: true,
        parkedTrimEligible: true,
      },
    },
  };
}

function merge(raw: unknown): SmartTrimSettings {
  const d = defaultSmartTrimSettings();
  if (!raw || typeof raw !== 'object') return d;
  const o = raw as Record<string, unknown>;
  const pv = (o.proxy && typeof o.proxy === 'object' ? o.proxy : {}) as Record<string, unknown>;
  const ev = (o.exalt && typeof o.exalt === 'object' ? o.exalt : {}) as Record<string, unknown>;

  const num = (v: unknown, fallback: number) =>
    typeof v === 'number' && Number.isFinite(v) ? v : fallback;

  const bool = (v: unknown, fallback: boolean) =>
    typeof v === 'boolean' ? v : fallback;

  return {
    version: SMART_TRIM_VERSION,
    proxy: {
      enabled: bool(pv.enabled, d.proxy.enabled),
      checkIntervalMs: Math.max(5000, Math.floor(num(pv.checkIntervalMs, d.proxy.checkIntervalMs))),
      rssBytesThreshold: Math.max(0, num(pv.rssBytesThreshold, d.proxy.rssBytesThreshold)),
      packetRateThreshold: Math.max(0, num(pv.packetRateThreshold, d.proxy.packetRateThreshold)),
      minTrimIntervalMs: Math.max(10_000, Math.floor(num(pv.minTrimIntervalMs, d.proxy.minTrimIntervalMs))),
      trimPackets: bool(pv.trimPackets, d.proxy.trimPackets),
      trimPacketLab: bool(pv.trimPacketLab, d.proxy.trimPacketLab),
      trimWorldSnapshot: bool(pv.trimWorldSnapshot, d.proxy.trimWorldSnapshot),
      runGcHint: bool(pv.runGcHint, d.proxy.runGcHint),
    },
    exalt: {
      enabled: bool(ev.enabled, d.exalt.enabled),
      checkIntervalMs: Math.max(5000, Math.floor(num(ev.checkIntervalMs, d.exalt.checkIntervalMs))),
      workingSetBytesPerProcessThreshold: Math.max(
        0,
        num(ev.workingSetBytesPerProcessThreshold, d.exalt.workingSetBytesPerProcessThreshold),
      ),
      periodicTrim: bool(ev.periodicTrim, d.exalt.periodicTrim),
      minTrimIntervalMs: Math.max(60_000, Math.floor(num(ev.minTrimIntervalMs, d.exalt.minTrimIntervalMs))),
      requireMemoryLoadPercent: Math.min(
        100,
        Math.max(0, num(ev.requireMemoryLoadPercent, d.exalt.requireMemoryLoadPercent)),
      ),
      maxCpuPercentForTrim: Math.max(0, num(ev.maxCpuPercentForTrim, d.exalt.maxCpuPercentForTrim)),
      minWorkingSetBytesBeforeTrim: Math.max(
        0,
        num(ev.minWorkingSetBytesBeforeTrim, d.exalt.minWorkingSetBytesBeforeTrim),
      ),
      trimParentWs: bool(ev.trimParentWs, d.exalt.trimParentWs),
      trimChildWs: bool(ev.trimChildWs, d.exalt.trimChildWs),
      trimRolePolicy: (() => {
        const rp = ev.trimRolePolicy;
        const dm = d.exalt.trimRolePolicy ?? {
          activeTrimEligible: false,
          backgroundTrimEligible: true,
          parkedTrimEligible: true,
        };
        if (!rp || typeof rp !== 'object') return dm;
        const rr = rp as Record<string, unknown>;
        return {
          activeTrimEligible: bool(rr.activeTrimEligible, dm.activeTrimEligible),
          backgroundTrimEligible: bool(rr.backgroundTrimEligible, dm.backgroundTrimEligible),
          parkedTrimEligible: bool(rr.parkedTrimEligible, dm.parkedTrimEligible),
        };
      })(),
    },
  };
}

export function smartTrimSettingsPath(): string {
  return join(getRealmengineDocumentsDir(), FILE);
}

export function loadSmartTrimSettings(): SmartTrimSettings {
  const dir = getRealmengineDocumentsDir();
  const p = join(dir, FILE);
  try {
    if (!existsSync(dir)) mkdirSync(dir, { recursive: true });
    if (!existsSync(p)) return merge(undefined);
    return merge(JSON.parse(readFileSync(p, 'utf8')));
  } catch {
    return merge(undefined);
  }
}

export function saveSmartTrimSettings(patch: Partial<SmartTrimSettings>): SmartTrimSettings {
  const cur = loadSmartTrimSettings();
  const n = merge({
    ...cur,
    ...patch,
    proxy: patch.proxy ? { ...cur.proxy, ...patch.proxy } : cur.proxy,
    exalt: patch.exalt ? { ...cur.exalt, ...patch.exalt } : cur.exalt,
  });
  const dir = getRealmengineDocumentsDir();
  if (!existsSync(dir)) mkdirSync(dir, { recursive: true });
  writeFileSync(smartTrimSettingsPath(), JSON.stringify(n, null, 2), 'utf8');
  return n;
}
