import type { ClientConnection } from '../proxy/ClientConnection.js';

export const HUB_MAPS = new Set([
  'Nexus', 'Vault', 'Cloth Bazaar',
  'Guild Hall', 'Guild Hall 2', 'Guild Hall 3', 'Guild Hall 4', 'Guild Hall 5',
  'Nexus Explanation', 'Vault Explanation', 'Guild Explanation',
]);

export function isHubMap(mapName: string): boolean {
  const name = mapName.trim();
  return !name || HUB_MAPS.has(name);
}

export function mapNameFromPacket(packet: { data?: { name?: string; displayName?: string } }): string {
  return String(packet.data?.name ?? packet.data?.displayName ?? '').trim();
}

export type CombatArmOptions = {
  arm: () => void;
  disarm: () => void;
  /** Wait after combat MAPINFO before arming (portal load is heavy). */
  settleMs?: number;
  /** Require proxy NEWTICK objectId + maxHealth before arming. */
  requireWirePlayer?: boolean;
};

/** Arm combat DLL features in realm/dungeon; disarm in hubs. Survives portal reconnects. */
export function createCombatMapArmer(opts: CombatArmOptions) {
  const settleMs = opts.settleMs ?? 0;
  const requireWirePlayer = opts.requireWirePlayer ?? false;

  let armed = false;
  let inCombatMap = false;
  let mapEnteredAt = 0;
  let wireReady = false;
  let settleTimer: ReturnType<typeof setTimeout> | null = null;

  function clearSettleTimer() {
    if (settleTimer) {
      clearTimeout(settleTimer);
      settleTimer = null;
    }
  }

  function disarm() {
    clearSettleTimer();
    armed = false;
    wireReady = false;
    opts.disarm();
  }

  function arm() {
    clearSettleTimer();
    if (armed) return;
    armed = true;
    opts.arm();
  }

  function tryArm(pluginEnabled: boolean) {
    if (!pluginEnabled || armed || !inCombatMap) return;
    if (requireWirePlayer && !wireReady) return;
    if (settleMs > 0 && Date.now() - mapEnteredAt < settleMs) return;
    arm();
  }

  function scheduleSettleTry(pluginEnabled: boolean) {
    if (!pluginEnabled || armed || !inCombatMap || settleMs <= 0) return;
    if (settleTimer) return;
    const remaining = settleMs - (Date.now() - mapEnteredAt);
    settleTimer = setTimeout(() => {
      settleTimer = null;
      tryArm(pluginEnabled);
    }, Math.max(0, remaining));
  }

  function onMapInfo(mapName: string, pluginEnabled: boolean) {
    const combat = !isHubMap(mapName);
    if (!combat) {
      inCombatMap = false;
      mapEnteredAt = 0;
      if (pluginEnabled) disarm();
      return;
    }

    const entering = !inCombatMap;
    inCombatMap = true;
    if (entering) {
      mapEnteredAt = Date.now();
      armed = false;
      wireReady = false;
    }

    if (!pluginEnabled) return;
    tryArm(pluginEnabled);
    scheduleSettleTry(pluginEnabled);
  }

  function onNewTick(client: ClientConnection, pluginEnabled: boolean) {
    if (!inCombatMap || armed) return;
    if (client.objectId > 0 && (client.playerData?.maxHealth ?? 0) > 0) {
      wireReady = true;
    }
    if (!pluginEnabled) return;
    tryArm(pluginEnabled);
    scheduleSettleTry(pluginEnabled);
  }

  function onDisconnect() {
    inCombatMap = false;
    mapEnteredAt = 0;
    disarm();
  }

  function onEnabledChange(enabled: boolean) {
    if (!enabled) {
      disarm();
      return;
    }
    if (inCombatMap) {
      tryArm(true);
      scheduleSettleTry(true);
    } else {
      opts.disarm();
    }
  }

  return {
    onMapInfo,
    onNewTick,
    onDisconnect,
    onEnabledChange,
    isInCombatMap: () => inCombatMap,
    isArmed: () => armed,
  };
}
