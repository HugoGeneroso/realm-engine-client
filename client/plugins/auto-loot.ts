/**
 * Auto Loot — automatically picks loot out of nearby bags based on tier / UT / ST
 * / potion rules, with quickslot stacking, optional stat-pot autodrink, a manual
 * potion guard, a bag-appeared notifier, and "big loot bags".
 *
 * This file is the plugin entry point: it wires the focused modules under
 * `plugins/auto-loot/` together and registers the packet hooks. The actual logic
 * lives in those modules.
 */

import type { PluginContext } from '../src/plugins/PluginContext.js';
import { LootCatalog } from './auto-loot/catalog.js';
import { AutoLootSettings } from './auto-loot/settings.js';
import { StateStore } from './auto-loot/state.js';
import { LootRules } from './auto-loot/loot-rules.js';
import { BagScanner, registerBigBags } from './auto-loot/bags.js';
import { ManualPotionGuard } from './auto-loot/manual-potion-guard.js';
import { LootEngine } from './auto-loot/engine.js';
import { BAG_TYPES } from './auto-loot/constants.js';

export function register(ctx: PluginContext) {
  ctx.name = 'Auto Loot';
  ctx.category = 'automation';

  const catalog = new LootCatalog(ctx);

  const settings = new AutoLootSettings(ctx);
  settings.reloadLists();
  settings.register();

  const store = new StateStore();
  const rules = new LootRules(ctx, settings, catalog);
  const bags = new BagScanner(ctx, settings, catalog);
  const guard = new ManualPotionGuard(ctx, settings, store);
  const engine = new LootEngine(ctx, settings, catalog, store, rules, bags);

  registerBigBags(ctx, settings);

  // Manual potion guard: block/observe the player's own potion & quickslot packets.
  ctx.hookAllPackets((client, packet, fromClient) => {
    guard.handleOutgoingPacket(client, packet, fromClient);
  });

  ctx.hookPacket('NEWTICK', (client) => {
    engine.tryAutoLoot(client);
  });

  ctx.hookPacket('MAPINFO', (client) => {
    store.reset(client);
  });

  ctx.on('clientConnected', (client) => {
    store.reset(client);
  });

  ctx.log(`Loaded ${catalog.size} lootable item defs across ${BAG_TYPES.size} bag types.`);
}
