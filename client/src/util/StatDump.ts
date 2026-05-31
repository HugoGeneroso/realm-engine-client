import { appendFileSync } from 'fs';
import { join } from 'path';
import { tmpdir } from 'os';

/**
 * Ground-truth wire-stat dump for resolving stat-type drift.
 *
 * The proxy IS the wire deserializer — it reads the raw `(statId, value)` pairs
 * straight off STATUS/NEWTICK before any of our (possibly stale) naming is
 * applied. Dumping them here captures exactly what the live game sends, so the
 * stat IDs in `StatType.ts` / `stat-types.json` can be regenerated from reality
 * instead of hand-maintained.
 *
 * Off by default. Enable with `RE_STAT_DUMP=1` in the proxy's environment.
 * Writes one JSON line per local-player status to
 * `<tmp>/realm-engine-statdump.jsonl`.
 *
 * To resolve base-vs-effective defense: open the in-game character panel, read
 * the displayed DEF, then compare against this dump —
 *   - if `DEFENSE(21)` alone == panel DEF  → stat 21 is already effective
 *     (so `pd.defense + pd.defenseBonus` double-counts — the AutoNexus bug)
 *   - if `DEFENSE(21) + DEFENSE_BOOST(49)` == panel DEF → 21 is base, the
 *     add is correct, and the bug is elsewhere (e.g. the exalt subtraction).
 */

const STAT_DUMP_FILE = join(tmpdir(), 'realm-engine-statdump.jsonl');
const ENABLED = process.env.RE_STAT_DUMP === '1';

/** RealmShark-authoritative labels for the combat-relevant stats (eyeballing only — raw id is authoritative). */
const LABELS: Record<number, string> = {
  0: 'MAX_HP(0)', 1: 'HP(1)', 3: 'MAX_MP(3)', 4: 'MP(4)', 7: 'LEVEL(7)',
  20: 'ATTACK(20)', 21: 'DEFENSE(21)', 22: 'SPEED(22)',
  26: 'VITALITY(26)', 27: 'WISDOM(27)', 28: 'DEXTERITY(28)',
  46: 'MAXHP_BOOST(46)', 47: 'MAXMP_BOOST(47)', 48: 'ATTACK_BOOST(48)', 49: 'DEFENSE_BOOST(49)',
  50: 'SPEED_BOOST(50)', 51: 'VIT_BOOST(51)', 52: 'WIS_BOOST(52)', 53: 'DEX_BOOST(53)',
  105: 'EXALTED_ATT(105)', 106: 'EXALTED_DEF(106)', 107: 'EXALTED_SPD(107)', 108: 'EXALTED_VIT(108)',
  109: 'EXALTED_DEX(109)', 110: 'EXALTED_WIS(110)', 111: 'EXALTED_HP(111)', 112: 'EXALTED_MP(112)',
};

let warned = false;

/**
 * Append the local player's raw wire stats to the dump file. No-op unless
 * `RE_STAT_DUMP=1`. `source` distinguishes UPDATE vs NEWTICK batches.
 */
export function dumpLocalPlayerStats(
  statDataArray: Array<{ id: number; value: number | string }> | undefined | null,
  source: 'UPDATE' | 'NEWTICK',
): void {
  if (!ENABLED || !Array.isArray(statDataArray) || statDataArray.length === 0) return;
  try {
    // Both the labelled view (for humans) and the raw id->value map (authoritative).
    const labelled: Record<string, number | string> = {};
    const raw: Record<string, number | string> = {};
    for (const s of statDataArray) {
      const id = Number(s.id);
      raw[String(id)] = s.value;
      labelled[LABELS[id] ?? `id_${id}`] = s.value;
    }
    const line = JSON.stringify({ t: new Date().toISOString(), source, labelled, raw }) + '\n';
    appendFileSync(STAT_DUMP_FILE, line);
    if (!warned) {
      warned = true;
      // eslint-disable-next-line no-console
      console.log(`[StatDump] RE_STAT_DUMP active → ${STAT_DUMP_FILE}`);
    }
  } catch {
    /* never let diagnostics break the proxy */
  }
}
