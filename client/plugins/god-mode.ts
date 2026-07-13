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

// Effects (stat 29) bits we strip from NEWTICK while god mode is on.
// Bleeding (0x8000) is the only damage-over-time debuff the server can apply
// without a fresh projectile hit; we clean it client-side the same way
// Anti-Debuffs does (the game only sends changed stat deltas, so the client
// retains the cleaned value).
const BIT_BLEEDING = 0x00008000;

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

  // Hidden constant used by the warnWhen below (you cannot disable the primary
  // server-damage block without risking server-side death — the DLL
  // invincibility is client-side only).
  ctx.registerSetting('blockRequired', {
    label: 'Required', type: 'boolean', value: true, hidden: true,
  });

  ctx.registerSetting('blockServerDamage', {
    label: 'Block incoming DAMAGE to you',
    type: 'boolean',
    value: true,
    warnWhen: {
      key: 'blockRequired',
      cmp: 'neq',
      message: 'Turning off the primary server-damage block lets server-side damage (and death) through. Keep on unless you know why.',
    },
  });

  ctx.registerSetting('restoreHpOnNewtick', {
    label: 'Restore HP on NEWTICK if server lowered it',
    type: 'boolean',
    value: true,
  });

  ctx.registerSetting('healToFull', {
    label: 'Heal to full on NEWTICK',
    type: 'boolean',
    value: false,
    advanced: true,
  });

  ctx.registerSetting('stripBleeding', {
    label: 'Strip Bleeding condition effect on NEWTICK',
    type: 'boolean',
    value: true,
    advanced: true,
  });

  ctx.registerSetting('notifyOnDeath', {
    label: 'Warn if a DEATH packet slips through',
    type: 'boolean',
    value: true,
    advanced: true,
  });

  ctx.registerSetting('toggleGod', {
    label: 'Toggle God Mode',
    type: 'button',
    value: null,
  }, () => {
    ctx.enabled = !ctx.enabled;
  });

  ctx.registerSetting('hotkey', {
    label: 'Hotkey',
    type: 'text',
    value: 'G',
    hotkeyFor: 'toggleGod',
  });

  function publishStatus(): void {
    ctx.broadcastData('god:status', {
      enabled: ctx.enabled,
      dllInvincible: ctx.enabled,
      ghostHitOff: ctx.enabled,
      healToFull: ctx.getSetting<boolean>('healToFull'),
      stripBleeding: ctx.getSetting<boolean>('stripBleeding'),
    });
  }

  function applyEnabled(enabled: boolean): void {
    syncDll(enabled);
    publishStatus();
  }

  // Re-push DLL state when the bridge reloads config / auth.
  ctx.registerDllResync(() => {
    syncDll(ctx.enabled);
  });

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
    if (!ctx.enabled) return;
    if (!packet.isDefined || !Array.isArray(packet.data.statuses)) return;

    const restoreHp = ctx.getSetting<boolean>('restoreHpOnNewtick');
    const healToFull = ctx.getSetting<boolean>('healToFull');
    const stripBleeding = ctx.getSetting<boolean>('stripBleeding');
    const maxHp = client.playerData?.maxHealth ?? 0;
    const floor = readGuardedHp(client);
    let changed = false;

    for (const status of packet.data.statuses as Array<{ objectId?: number; data?: Array<{ id: number; value: unknown }> }>) {
      if (status.objectId !== client.objectId || !status.data) continue;
      for (const stat of status.data) {
        if (stat.id === StatType.HP) {
          const incoming = Math.trunc(Number(stat.value));
          if (!Number.isFinite(incoming)) continue;
          if (restoreHp && floor > 0 && incoming < floor) {
            stat.value = floor;
            changed = true;
          } else if (healToFull && maxHp > 0 && incoming < maxHp) {
            stat.value = maxHp;
            guardedHp.set(client, maxHp);
            changed = true;
          } else if (incoming > 0) {
            guardedHp.set(client, incoming);
          }
        } else if (stat.id === StatType.Effects) {
          if (!stripBleeding) continue;
          const v = Number(stat.value) | 0;
          const cleaned = v & ~BIT_BLEEDING;
          if (cleaned !== v) { stat.value = cleaned; changed = true; }
        }
      }
    }

    if (changed) packet.modified = true;
  }, { prepend: true });

  // Map changes (teleport / reconnect) can leave a stale HP floor — reset it so
  // the next NEWTICK re-establishes the floor from fresh server data.
  for (const evt of ['GOTOACK', 'RECONNECT'] as const) {
    ctx.hookPacket(evt, (client) => { guardedHp.delete(client); });
  }

  ctx.hookPacket('CREATESUCCESS', (client, packet) => {
    if (!packet.isDefined) return;
    guardedHp.delete(client);
  });

  // DEATH is intentionally NOT blocked (blocking it desyncs the client). While
  // god mode is on, a DEATH packet means server-side damage leaked past our
  // proxy guards — surface it so the user knows protection failed.
  ctx.hookPacket('DEATH', (client, packet) => {
    if (!ctx.enabled || !ctx.getSetting<boolean>('notifyOnDeath')) return;
    if (!packet.isDefined) return;
    const name = client.playerData?.name ? ` (${client.playerData.name})` : '';
    ctx.dashboardLog(`DEATH packet received while God Mode ON${name} — server-side damage may have bypassed proxy protection.`);
    ctx.broadcastData('god:death', { name: client.playerData?.name ?? '', time: Date.now() });
  });

  ctx.hookCommand('god', (client, _cmd, args) => {
    const arg = (args[0] ?? '').toLowerCase();
    if (arg === 'off' || arg === '0') ctx.enabled = false;
    else if (arg === 'on' || arg === '1') ctx.enabled = true;
    else ctx.enabled = !ctx.enabled;
    ctx.sendNotification(client, 'GodMode', `God Mode ${ctx.enabled ? 'ON' : 'OFF'}`);
  });

  ctx.log('Loaded — blocks hit packets, restores HP, strips bleeding, and reports leaked deaths (collider alone does not prevent damage).');
}
