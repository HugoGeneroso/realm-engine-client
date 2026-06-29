import type { PluginContext } from './PluginContext.js';
import type { ClientConnection } from '../proxy/ClientConnection.js';
import type { Packet } from '../packets/Packet.js';
import type { TrackedEntity } from '../state/GameWorldState.js';

const SHOOT_COOLDOWN_MS = 160;
const MUZZLE_OFFSET = 0.3;
const INT32_MAX = 0x7fffffff;

/** Wire layout after 5-byte header (len int32 + id byte). */
const OFF_TIME = 5;
const OFF_SHOT_ID = 9;
const OFF_PROJ_X = 14;
const OFF_PROJ_Y = 18;
const OFF_ANGLE = 22;
const OFF_BULLET_ID = 26;
const MIN_SHOOT_LEN = 37;

const activeChecks: Array<() => boolean> = [];
let installed = false;

type AimPoint = { x: number; y: number; objectId: number };

type ClientShootState = {
  template: Buffer | null;
  nextShotId: number;
  nextBulletId: number;
  lastInjectMs: number;
};

const shootState = new Map<ClientConnection, ClientShootState>();
const injectedRaw = new WeakSet<Buffer>();

function getShootState(client: ClientConnection): ClientShootState {
  let st = shootState.get(client);
  if (!st) {
    st = { template: null, nextShotId: 1, nextBulletId: 1, lastInjectMs: 0 };
    shootState.set(client, st);
  }
  return st;
}

function isProxyAimActive(): boolean {
  return activeChecks.some((fn) => fn());
}

function shootPacketTime(client: ClientConnection): number | null {
  if (client.relativeTime !== 0) {
    const t = Math.trunc(client.time);
    if (Number.isFinite(t) && t >= -INT32_MAX && t <= INT32_MAX) return t;
  }
  if (client.serverConnectedAt > 0) {
    const t = Math.trunc(client.gameTime);
    if (Number.isFinite(t) && t >= 0 && t <= INT32_MAX) return t;
  }
  const serverRt = Math.trunc(client.lastServerRealTimeMs);
  if (serverRt > 0 && serverRt <= INT32_MAX) return serverRt;
  return null;
}

function playerPos(client: ClientConnection): { x: number; y: number } | null {
  const pos = client.playerData?.pos;
  if (!pos || !Number.isFinite(pos.x) || !Number.isFinite(pos.y)) return null;
  return pos;
}

function pickAimPoint(
  ctx: PluginContext,
  client: ClientConnection,
): AimPoint | null {
  const ws = ctx.getWorldState(client);
  const gd = ctx.gameData;
  const origin = playerPos(client);
  if (!ws || !gd || !origin) return null;

  const nearest = ws.getNearestEnemy(gd, origin, { maxDistance: 30 }, client.objectId);
  if (nearest) return { x: nearest.x, y: nearest.y, objectId: nearest.objectId };

  let best: AimPoint | null = null;
  let bestD2 = 30 * 30;
  ws.forEachEntity((entity: TrackedEntity) => {
    if (!entity.objectId || entity.objectId === client.objectId) return;
    const cat = gd.getObjectCategory(entity.objectType);
    if (cat === 'Player' || cat === 'Pet' || cat === 'Portal' || cat === 'Container' || cat === 'Projectile') {
      return;
    }
    if (entity.objectType < 256) return;
    const x = entity.pos?.x;
    const y = entity.pos?.y;
    if (!Number.isFinite(x) || !Number.isFinite(y)) return;
    const d2 = (x - origin.x) ** 2 + (y - origin.y) ** 2;
    if (d2 < bestD2) {
      bestD2 = d2;
      best = { x, y, objectId: entity.objectId };
    }
  });
  return best;
}

function readLocation(value: unknown): { x: number; y: number } | null {
  if (!value || typeof value !== 'object') return null;
  const x = Number((value as { x?: unknown }).x);
  const y = Number((value as { y?: unknown }).y);
  if (!Number.isFinite(x) || !Number.isFinite(y)) return null;
  return { x, y };
}

function patchShootRaw(
  raw: Buffer,
  angle: number,
  playerPosition: { x: number; y: number },
  time?: number,
  shotId?: number,
  bulletId?: number,
): Buffer | null {
  if (raw.length < MIN_SHOOT_LEN) return null;
  if (!Number.isFinite(angle)) return null;

  const out = Buffer.from(raw);
  const projX = playerPosition.x + Math.cos(angle) * MUZZLE_OFFSET;
  const projY = playerPosition.y + Math.sin(angle) * MUZZLE_OFFSET;
  out.writeFloatBE(projX, OFF_PROJ_X);
  out.writeFloatBE(projY, OFF_PROJ_Y);
  out.writeFloatBE(angle, OFF_ANGLE);
  if (time != null && Number.isFinite(time)) out.writeInt32BE(Math.trunc(time), OFF_TIME);
  if (shotId != null) out.writeUInt16BE(shotId & 0xffff, OFF_SHOT_ID);
  if (bulletId != null) out.writeUInt8(bulletId & 0xff, OFF_BULLET_ID);
  return out;
}

function rememberTemplate(client: ClientConnection, packet: Packet): void {
  const raw = packet.rawBytes;
  if (!packet.isDefined || !raw || raw.length < MIN_SHOOT_LEN) return;
  if (injectedRaw.has(raw)) return;

  const st = getShootState(client);
  st.template = Buffer.from(raw);
  st.nextShotId = (raw.readUInt16BE(OFF_SHOT_ID) + 1) & 0xffff;
  if (st.nextShotId === 0) st.nextShotId = 1;
  st.nextBulletId = (raw.readUInt8(OFF_BULLET_ID) + 1) & 0xff;
  if (st.nextBulletId === 0) st.nextBulletId = 1;
}

function sendRawPlayerShoot(client: ClientConnection, raw: Buffer): void {
  const pkt: Packet = {
    id: raw[4] ?? 30,
    name: 'PLAYERSHOOT',
    direction: 'client',
    send: true,
    modified: false,
    data: {},
    rawBytes: raw,
    unreadData: Buffer.alloc(0),
    isDefined: false,
    bodyLength: raw.length - 5,
  };
  injectedRaw.add(raw);
  client.sendToServer(pkt);
}

function tryInjectAutoFire(
  client: ClientConnection,
  origin: { x: number; y: number },
  target: AimPoint,
): void {
  const st = getShootState(client);
  if (!st.template) return;

  const pktTime = shootPacketTime(client);
  if (pktTime == null || client.relativeTime === 0) return;

  const now = Date.now();
  if (now - st.lastInjectMs < SHOOT_COOLDOWN_MS) return;

  const dx = target.x - origin.x;
  const dy = target.y - origin.y;
  if (Math.abs(dx) < 1e-6 && Math.abs(dy) < 1e-6) return;

  const angle = Math.atan2(dy, dx);
  const patched = patchShootRaw(
    st.template,
    angle,
    origin,
    pktTime,
    st.nextShotId,
    st.nextBulletId,
  );
  if (!patched) return;

  st.nextShotId = (st.nextShotId + 1) & 0xffff;
  if (st.nextShotId === 0) st.nextShotId = 1;
  st.nextBulletId = (st.nextBulletId + 1) & 0xff;
  if (st.nextBulletId === 0) st.nextBulletId = 1;
  st.lastInjectMs = now;

  sendRawPlayerShoot(client, patched);
}

function redirectOutgoingShoot(
  ctx: PluginContext,
  client: ClientConnection,
  packet: Packet,
): void {
  if (!isProxyAimActive() || !packet.isDefined) return;
  if (packet.data.time == null || packet.data.shotId == null) return;

  const target = pickAimPoint(ctx, client);
  if (!target) return;

  const currentAngle = Number(packet.data.angle);
  const playerPosition =
    readLocation(packet.data.playerPosition) ??
    (() => {
      const projectilePosition = readLocation(packet.data.projectilePosition);
      if (!projectilePosition || !Number.isFinite(currentAngle)) return null;
      return {
        x: projectilePosition.x - Math.cos(currentAngle) * MUZZLE_OFFSET,
        y: projectilePosition.y - Math.sin(currentAngle) * MUZZLE_OFFSET,
      };
    })() ??
    playerPos(client);

  if (!playerPosition) return;

  const dx = target.x - playerPosition.x;
  const dy = target.y - playerPosition.y;
  if (Math.abs(dx) < 1e-6 && Math.abs(dy) < 1e-6) return;

  const angle = Math.atan2(dy, dx);
  const raw = packet.rawBytes;
  if (!raw || raw.length < MIN_SHOOT_LEN) return;

  const patched = patchShootRaw(raw, angle, playerPosition);
  if (!patched) return;

  packet.rawBytes = patched;
  packet.data.angle = angle;
  packet.data.projectilePosition = {
    x: playerPosition.x + Math.cos(angle) * MUZZLE_OFFSET,
    y: playerPosition.y + Math.sin(angle) * MUZZLE_OFFSET,
  };
}

/**
 * MultiTool-style aim: patch PLAYERSHOOT on raw wire bytes (never re-serialize).
 * Auto-fire clones the last real shot template with updated time/ids/angle.
 */
export function installCombatProxyAim(ctx: PluginContext, isActive: () => boolean): void {
  activeChecks.push(isActive);
  if (installed) return;
  installed = true;

  ctx.hookPacket('PLAYERSHOOT', (client, packet) => {
    rememberTemplate(client, packet);
    redirectOutgoingShoot(ctx, client, packet);
  }, { prepend: true });

  ctx.on('clientDisconnected', (client) => {
    shootState.delete(client);
  });
}
