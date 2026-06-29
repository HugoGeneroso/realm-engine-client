import type { PluginContext } from './PluginContext.js';
import type { GameDataLoader } from '../game-data/GameDataLoader.js';
import type { TrackedEntity } from '../state/GameWorldState.js';
import { sendDllFeature } from '../bridge/DllFeatureBus.js';
import { StatType } from '../constants/StatType.js';

const activeChecks: Array<() => boolean> = [];
let installed = false;

function isWireActive(): boolean {
  return activeChecks.some((fn) => fn());
}

function statFromEntity(entity: TrackedEntity, statId: number): number | null {
  const raw = entity.stats?.[String(statId)];
  const n = Number(raw);
  return Number.isFinite(n) ? Math.trunc(n) : null;
}

function resolveHp(
  objectType: number,
  entity: TrackedEntity,
  gd: GameDataLoader | null,
): { hp: number; maxHp: number } {
  let hp = statFromEntity(entity, StatType.HP);
  let maxHp = statFromEntity(entity, StatType.MaxHP);
  const def = gd?.getObject(objectType);

  if (!maxHp || maxHp <= 0) maxHp = def?.maxHp ?? null;
  if (!hp || hp <= 0) hp = maxHp ?? null;
  if ((!hp || !maxHp) && def?.maxHp && def.maxHp > 0) {
    hp = def.maxHp;
    maxHp = def.maxHp;
  }

  const sizeVal = statFromEntity(entity, StatType.Size);
  if ((!hp || !maxHp) && sizeVal && sizeVal >= 10 && objectType >= 256) {
    hp = sizeVal;
    maxHp = sizeVal;
  }

  return { hp: hp ?? 0, maxHp: maxHp ?? hp ?? 0 };
}

function isWireEnemy(
  gd: GameDataLoader,
  entity: TrackedEntity,
  localOid: number,
): boolean {
  if (!entity.objectId || entity.objectId === localOid) return false;
  const objectType = entity.objectType;
  if (!objectType || objectType < 256) return false;

  const cat = gd.getObjectCategory(objectType);
  if (cat === 'Player' || cat === 'Pet' || cat === 'Portal' || cat === 'Container' || cat === 'Projectile') {
    return false;
  }

  const def = gd.getObject(objectType);
  if (def?.isEnemy || cat === 'Enemy') return true;

  const { hp, maxHp } = resolveHp(objectType, entity, gd);
  if (hp > 0 && maxHp > 0) return true;
  if (def?.maxHp && def.maxHp >= 25) return true;
  return cat === 'Other' && objectType >= 50000;
}

function isJunkEnemy(objectType: number, hp: number, maxHp: number): boolean {
  if (hp <= 0 && maxHp <= 0) return true;
  if (maxHp > 0 && maxHp < 25 && objectType < 50000) return true;
  return false;
}

function buildWireSnapshot(
  gd: GameDataLoader,
  localOid: number,
  origin: { x: number; y: number } | null,
  ws: NonNullable<ReturnType<PluginContext['getWorldState']>>,
): string {
  const parts: string[] = [];
  const seen = new Set<number>();

  const push = (entity: TrackedEntity, hp: number, maxHp: number) => {
    if (seen.has(entity.objectId)) return;
    const x = entity.pos?.x;
    const y = entity.pos?.y;
    if (!Number.isFinite(x) || !Number.isFinite(y)) return;
    if (isJunkEnemy(entity.objectType, hp, maxHp)) return;
    seen.add(entity.objectId);
    parts.push(`${entity.objectId},${entity.objectType},${x},${y},${hp},${maxHp}`);
  };

  ws.forEachEntity((entity: TrackedEntity) => {
    if (!isWireEnemy(gd, entity, localOid)) return;
    const { hp, maxHp } = resolveHp(entity.objectType, entity, gd);
    push(entity, hp > 0 ? hp : maxHp, maxHp > 0 ? maxHp : hp);
  });

  if (parts.length === 0 && origin) {
    const nearest = ws.getNearestEnemy(gd, origin, { maxDistance: 40 }, localOid);
    if (nearest) {
      const ent: TrackedEntity = {
        objectId: nearest.objectId,
        objectType: nearest.objectType,
        pos: { x: nearest.x, y: nearest.y },
        lastUpdate: Date.now(),
      };
      push(ent, nearest.hp, nearest.maxHp);
    }
  }

  return parts.slice(0, 48).join('|');
}

/** Feed proxy-visible enemies to the DLL when combat plugins are armed. */
export function installCombatWireEnemies(ctx: PluginContext, isActive: () => boolean): void {
  activeChecks.push(isActive);
  if (installed) return;
  installed = true;

  ctx.hookPacket('NEWTICK', (client, packet) => {
    if (!packet.isDefined) return;

    const pos = client.playerData?.pos;
    if (pos && Number.isFinite(pos.x) && Number.isFinite(pos.y)) {
      sendDllFeature('clientPosX', pos.x);
      sendDllFeature('clientPosY', pos.y);
    }

    if (!isWireActive()) {
      sendDllFeature('wireEnemySnapshot', '');
      return;
    }

    const gd = ctx.gameData;
    const ws = ctx.getWorldState(client);
    if (!gd || !ws) {
      sendDllFeature('wireEnemySnapshot', '');
      return;
    }

    const snapshot = buildWireSnapshot(gd, client.objectId, pos ?? null, ws);
    sendDllFeature('wireEnemySnapshot', snapshot);
  });

  ctx.on('clientDisconnected', () => {
    sendDllFeature('wireEnemySnapshot', '');
    sendDllFeature('clientPosX', 0);
    sendDllFeature('clientPosY', 0);
  });
}
