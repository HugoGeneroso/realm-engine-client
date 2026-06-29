import type { PluginContext } from '../src/plugins/PluginContext.js';
import {
  DEFENSE_UNSET,
  openShared,
  readPosition,
} from '../src/native/rotmg-shared.js';
import { sendDllFeature } from '../src/bridge/DllFeatureBus.js';
import { createCombatMapArmer, mapNameFromPacket, isHubMap } from '../src/plugins/combat-map-arm.js';
import { installCombatWireEnemies } from '../src/plugins/combat-wire-enemies.js';
import { installCombatProxyAim } from '../src/plugins/combat-proxy-aim.js';

/**
 * Auto-aim: enable/mode are driven over the DLL pipe.
 * Shared memory is only used for optional read-only telemetry like position polling.
 * Pushes defense and objectType from NEWTICK so native combat helpers use client-authoritative stats.
 */
export function register(ctx: PluginContext) {
  ctx.name = 'Auto Aim';
  ctx.category = 'combat';
  let loggedSharedOpen = false;
  let loggedBridgeTelemetry = false;

  let aimModeIdx = 0;
  let _lastWireStatsMs = 0;
  function isDllAimActive(): boolean {
    return ctx.enabled && combatArm.isInCombatMap() && combatArm.isArmed();
  }
  /** Proxy redirect — no 4s settle; server packet is the source of truth for aim. */
  function isProxyAimActive(): boolean {
    return ctx.enabled && combatArm.isInCombatMap();
  }

  function syncControlState() {
    sendDllFeature('autoAimMode', aimModeIdx);
    sendDllFeature('autoAimEnabled', isDllAimActive());
  }

  function syncProjectileNoclipState(forceOff = false) {
    sendDllFeature(
      'projectileNoclipEnabled',
      !forceOff && ctx.enabled && ctx.getSetting<boolean>('projectileNoclip') === true,
    );
  }

  ctx.registerSetting('aimMode', {
    label: 'Aim mode',
    type: 'select',
    value: 'player',
    options: [
      { label: 'Closest to player', value: 'player' },
      { label: 'Highest HP', value: 'hp' },
      { label: 'Closest to mouse', value: 'mouse' },
    ],
  }, (val: string) => {
    aimModeIdx = val === 'hp' ? 1 : val === 'mouse' ? 2 : 0;
    // #region agent log
    // #endregion
    syncControlState();
  });

  ctx.registerSetting('prioritizeBosses', {
    label: 'Prioritize bosses',
    type: 'boolean',
    value: false,
  }, (val: boolean) => {
    sendDllFeature('autoAimPrioritizeBosses', val);
  });

  ctx.registerSetting('ignoreWalls', {
    label: 'Ignore walls / no-HP-bar',
    type: 'boolean',
    value: true,
  }, (val: boolean) => {
    sendDllFeature('autoAimIgnoreWalls', val);
  });

  ctx.registerSetting('projectileNoclip', {
    label: 'Projectile noclip',
    type: 'boolean',
    value: false,
  }, () => {
    syncProjectileNoclipState();
  });

  function syncMenuSafeState() {
    sendDllFeature('autoAimEnabled', false);
    syncProjectileNoclipState(true);
  }

  const combatArm = createCombatMapArmer({
    arm: () => syncAllDllState(),
    disarm: () => syncMenuSafeState(),
    settleMs: 0,
    requireWirePlayer: true,
  });

  installCombatWireEnemies(ctx, () => ctx.enabled && combatArm.isInCombatMap());
  installCombatProxyAim(ctx, () => isProxyAimActive());

  ctx.onEnabledChange((enabled) => {
    combatArm.onEnabledChange(enabled);
    if (enabled && combatArm.isInCombatMap()) {
      syncAimModeIdx();
      syncControlState();
      syncFilterState();
    } else if (!enabled) {
      syncMenuSafeState();
    }
  });

  ctx.registerDllResync(() => {
    if (!ctx.enabled) {
      syncMenuSafeState();
      return;
    }
    if (combatArm.isInCombatMap()) {
      syncAllDllState();
    } else {
      syncMenuSafeState();
    }
  });

  ctx.on('clientDisconnected', () => {
    sendDllFeature('clientDefense', DEFENSE_UNSET);
    sendDllFeature('clientClassType', 0);
    sendDllFeature('clientHp', 0);
    sendDllFeature('clientMaxHp', 0);
    sendDllFeature('clientObjectId', 0);
    syncProjectileNoclipState(true);
    combatArm.onDisconnect();
  });

  let posTimer: ReturnType<typeof setInterval> | null = null;

  function syncAimModeIdx() {
    const m = ctx.getSetting<string>('aimMode');
    aimModeIdx = m === 'hp' ? 1 : m === 'mouse' ? 2 : 0;
  }

  function startPosPoll() {
    if (posTimer) return;
    posTimer = setInterval(() => {
      if (!ctx.enabled) return;
      const pos = readPosition();
      if (!pos) return;
      ctx.setData('internalPos', pos);
      ctx.broadcastData('internalPos', pos);
    }, 16);
  }

  function tryOpenTelemetry() {
    const opened = openShared();
    if (!loggedSharedOpen) {
      loggedSharedOpen = true;
      // #region agent log
      // #endregion
    }
    return opened;
  }

  let _lastAimSyncMs = 0;
  ctx.hookPacket('NEWTICK', (client, packet) => {
    if (!packet.isDefined) return;
    const pd = client.playerData;
    const def = pd.defense + pd.defenseBonus;
    const cls = pd.classType ?? 0;
    if (!loggedBridgeTelemetry) {
      loggedBridgeTelemetry = true;
      // #region agent log
      // #endregion
    }
    if (!posTimer && tryOpenTelemetry()) startPosPoll();
    const now = Date.now();
    if (now - _lastWireStatsMs >= 300) {
      _lastWireStatsMs = now;
      sendDllFeature('clientDefense', def);
      sendDllFeature('clientClassType', cls);
      sendDllFeature('clientMaxHp', pd.maxHealth);
      sendDllFeature('clientHp', pd.health > 0 ? pd.health : pd.maxHealth);
      if (client.objectId > 0) sendDllFeature('clientObjectId', client.objectId);
      const pos = pd.pos;
      if (pos && Number.isFinite(pos.x) && Number.isFinite(pos.y)) {
        sendDllFeature('clientPosX', pos.x);
        sendDllFeature('clientPosY', pos.y);
      }
    }
    combatArm.onNewTick(client, ctx.enabled);

    if (ctx.enabled && combatArm.isArmed() && now - _lastAimSyncMs >= 500) {
      _lastAimSyncMs = now;
      syncControlState();
    }
  });

  ctx.hookPacket('MAPINFO', (_client, packet) => {
    try {
      if (!packet.isDefined) return;
      const mapName = mapNameFromPacket(packet);
      combatArm.onMapInfo(mapName, ctx.enabled);
      if (ctx.enabled && !isHubMap(mapName)) {
        syncAimModeIdx();
        syncFilterState();
        if (combatArm.isArmed()) syncControlState();
      }
    } catch (err) {
      ctx.log('MAPINFO hook error: ' + (err as Error).message);
    }
  });

  function syncFilterState() {
    sendDllFeature('autoAimPrioritizeBosses', ctx.getSetting<boolean>('prioritizeBosses'));
    sendDllFeature('autoAimIgnoreWalls', ctx.getSetting<boolean>('ignoreWalls'));
  }

  function syncAllDllState() {
    syncAimModeIdx();
    syncControlState();
    syncFilterState();
    syncProjectileNoclipState();
    if (tryOpenTelemetry()) startPosPoll();
  }

  ctx.registerCleanup(() => {
    if (posTimer) {
      clearInterval(posTimer);
      posTimer = null;
    }
    sendDllFeature('autoAimEnabled', false);
    sendDllFeature('autoAimMode', 0);
    syncProjectileNoclipState(true);
    sendDllFeature('clientDefense', DEFENSE_UNSET);
    sendDllFeature('clientClassType', 0);
    sendDllFeature('clientHp', 0);
    sendDllFeature('clientMaxHp', 0);
    sendDllFeature('clientObjectId', 0);
    sendDllFeature('clientPosX', 0);
    sendDllFeature('clientPosY', 0);
    // Do not unmap shared memory — auto-dodge / auto-ability may still be using it.
  });
}
