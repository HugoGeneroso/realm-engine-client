import type { PluginContext } from '../src/plugins/PluginContext.js';
import { sendDllFeature } from '../src/bridge/DllFeatureBus.js';
import { createCombatMapArmer, mapNameFromPacket } from '../src/plugins/combat-map-arm.js';

/**
 * Auto Follow plugin.
 *
 * Follow is handled entirely inside the DLL: the dashboard only sends the
 * target name + active flag over IPC. The DLL resolves the entity each frame
 * and feeds DangerPlanner::SetExternalGoal, so XDodge's A* pursues the target
 * AND dodges projectiles in one motion (movement + dodging at all times).
 *
 * There is intentionally NO client-side movement here — no WASD/background-key
 * injection, no dogebawt memory writes, no pathfinding. Any such injection
 * fights the DLL's NativeMoveTo and causes the teleport/stutter behaviour.
 */
export function register(ctx: PluginContext) {
  ctx.name = 'Auto Follow';

  let followName = '';

  function pushState(activeInRealm = false) {
    const active = activeInRealm && ctx.enabled && followName.length > 0;
    sendDllFeature('followEntityName', followName);
    sendDllFeature('followEntityActive', active);
  }

  function syncMenuSafe() {
    sendDllFeature('followEntityName', followName);
    sendDllFeature('followEntityActive', false);
  }

  ctx.registerSetting('followName', {
    label: 'Follow Player',
    type: 'text',
    value: followName,
  }, (val: string) => {
    followName = (val || '').trim();
    ctx.log(followName ? `Follow target set: "${followName}"` : 'Follow disabled');
    pushState(false);
  });

  const combatArm = createCombatMapArmer({
    arm: () => pushState(true),
    disarm: () => syncMenuSafe(),
    settleMs: 3000,
    requireWirePlayer: true,
  });

  ctx.onEnabledChange((enabled) => {
    combatArm.onEnabledChange(enabled);
  });

  ctx.registerDllResync(() => syncMenuSafe());

  ctx.on('clientDisconnected', () => {
    sendDllFeature('followEntityActive', false);
    combatArm.onDisconnect();
  });

  ctx.hookPacket('NEWTICK', (client, packet) => {
    if (!packet.isDefined) return;
    combatArm.onNewTick(client, ctx.enabled);
  });

  ctx.hookPacket('MAPINFO', (_client, packet) => {
    try {
      if (!packet.isDefined) return;
      combatArm.onMapInfo(mapNameFromPacket(packet), ctx.enabled);
    } catch (err) {
      ctx.log('MAPINFO hook error: ' + (err as Error).message);
    }
  });

  ctx.registerCleanup(() => {
    sendDllFeature('followEntityActive', false);
  });

  ctx.log('Loaded - set "Follow Player" to enable internal (DLL) follow.');
}
