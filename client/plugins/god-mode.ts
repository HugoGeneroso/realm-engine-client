import type { PluginContext } from '../src/plugins/PluginContext.js';
import type { ClientConnection } from '../src/proxy/ClientConnection.js';
import type { Packet } from '../src/packets/Packet.js';
import { sendDllFeature } from '../src/bridge/DllFeatureBus.js';
import { StatType } from '../src/constants/StatType.js';

/**
 * God Mode — blocks damage at the proxy layer.
 *
 * Collider radius 0 only stops the *client* from detecting hits locally.
 * RotMG applies damage when the client sends PLAYERHIT / GROUNDDAMAGE / AOEACK
 * (C→S), or when the server pushes DAMAGE / HP drops in NEWTICK (S→C).
 *
 * This plugin suppresses those paths. The DLL also sets BINDBHJLPMG (short
 * invincibility flag) every frame while enabled.
 *
 * Disable Auto Dodge "Ghost-hit protection" is forced off while god mode is on
 * (GhostHit synthesises PLAYERHIT — the opposite of god mode).
 */

const guardedHp = new WeakMap<ClientConnection, number>();

function readGuardedHp(client: ClientConnection): number {
  const pd = client.playerData;
  if (pd.health > 0) return pd.health;
  return guardedHp.get(client) ?? 0;
}

function syncDll(enabled: boolean): void {
  sendDllFeature('godModeEnabled', enabled);
  if (enabled) {
    // GhostHit injects PLAYERHIT when it thinks we were hit — disable while invuln.
    sendDllFeature('xdodgeGhostHit', false);
  }
}

export function register(ctx: PluginContext) {
  ctx.name = 'God Mode';
  ctx.category = 'combat';

  ctx.registerSetting('blockOutgoing', {
    label: 'Block outgoing hit acks (PLAYERHIT / ground / AoE)',
    type: 'boolean',
    value: true,
  });

  ctx.registerSetting('blockServerDamage', {
    label: 'Block incoming DAMAGE to you',
    type: 'boolean',
    value: true,
  });

  ctx.registerSetting('restoreHpOnNewtick', {
    label: 'Restore HP on NEWTICK if server lowered it',
    type: 'boolean',
    value: true,
  });

  function applyEnabled(enabled: boolean): void {
    syncDll(enabled);
  }

  ctx.onEnabledChange(applyEnabled);

  ctx.on('clientConnected', () => {
    if (ctx.enabled) applyEnabled(true);
  });

  ctx.on('clientDisconnected', () => {
    syncDll(false);
  });

  ctx.registerCleanup(() => {
    syncDll(false);
  });

  function blockIfEnabled(packet: Packet): void {
    if (!ctx.enabled) return;
    packet.send = false;
  }

  // C→S — client telling server "I was hit". Block before Auto Nexus simulates damage.
  ctx.hookPacket('PLAYERHIT', (client, packet) => {
    if (!ctx.enabled || !ctx.getSetting<boolean>('blockOutgoing')) return;
    if (!packet.isDefined) return;
    blockIfEnabled(packet);
  }, { prepend: true });

  ctx.hookPacket('GROUNDDAMAGE', (_client, packet) => {
    if (!ctx.enabled || !ctx.getSetting<boolean>('blockOutgoing')) return;
    if (!packet.isDefined) return;
    blockIfEnabled(packet);
  }, { prepend: true });

  ctx.hookPacket('AOEACK', (_client, packet) => {
    if (!ctx.enabled || !ctx.getSetting<boolean>('blockOutgoing')) return;
    if (!packet.isDefined) return;
    blockIfEnabled(packet);
  }, { prepend: true });

  // S→C — server-applied damage lines.
  ctx.hookAllPackets((client, packet, fromClient) => {
    if (fromClient || !ctx.enabled || !ctx.getSetting<boolean>('blockServerDamage')) return;
    if (packet.name !== 'DAMAGE' || !packet.isDefined) return;
    const targetId = Number(packet.data.targetId);
    if (targetId !== client.objectId) return;
    packet.send = false;
  });

  ctx.hookPacket('NEWTICK', (client, packet) => {
    if (!ctx.enabled || !ctx.getSetting<boolean>('restoreHpOnNewtick')) return;
    if (!packet.isDefined || !Array.isArray(packet.data.statuses)) return;

    const floor = readGuardedHp(client);
    let changed = false;

    for (const status of packet.data.statuses as Array<{ objectId?: number; data?: Array<{ id: number; value: unknown }> }>) {
      if (status.objectId !== client.objectId || !status.data) continue;
      for (const stat of status.data) {
        if (stat.id !== StatType.HP) continue;
        const incoming = Math.trunc(Number(stat.value));
        if (!Number.isFinite(incoming)) continue;
        if (floor > 0 && incoming < floor) {
          stat.value = floor;
          changed = true;
        } else if (incoming > 0) {
          guardedHp.set(client, incoming);
        }
      }
    }

    if (changed) packet.modified = true;
  }, { prepend: true });

  ctx.hookPacket('CREATESUCCESS', (client, packet) => {
    if (!packet.isDefined) return;
    guardedHp.delete(client);
  });

  ctx.log('Loaded — blocks hit packets and restores HP (collider alone does not prevent damage).');
}
