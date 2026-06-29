import net from 'net';
import { EventEmitter } from 'events';
import { ClientConnection } from './ClientConnection.js';
import { PacketFactory } from '../packets/PacketFactory.js';
import { type Packet } from '../packets/Packet.js';
import { State } from '../state/State.js';
import { Logger } from '../util/Logger.js';
import { LaunchTargetResolver } from './LaunchTargetResolver.js';
import { getServerDirectory, type ServerDirectory } from '../services/ServerDirectory.js';

export type ProxyOptions = {
  serverDirectory?: ServerDirectory;
};

let _clientIdSeq = 0;
function generateClientId(): string {
  return 'c' + (++_clientIdSeq) + '_' + Date.now().toString(36);
}

export type PacketHandler = (client: ClientConnection, packet: Packet) => void;
export type CommandHandler = (client: ClientConnection, command: string, args: string[]) => boolean | void;

/**
 * Central MITM proxy. Listens on 127.0.0.1:2050, manages hook registry,
 * fires packet events, and tracks connection state across reconnects.
 */
export class Proxy extends EventEmitter {
  private readonly launchTarget: LaunchTargetResolver;

  private listener: net.Server | null = null;
  private states = new Map<string, State>();

  // Hook registries
  private packetHooks = new Map<string, PacketHandler[]>();   // packetName -> handlers
  private commandHooks = new Map<string, CommandHandler[]>(); // command -> handlers

  // Track which hooks belong to which plugin for unloading
  private pluginHooks = new Map<string, { packets: Map<string, PacketHandler[]>; commands: Map<string, CommandHandler[]> }>();


  constructor(
    public readonly packetFactory: PacketFactory,
    options: ProxyOptions = {},
  ) {
    super();
    const directory = options.serverDirectory ?? getServerDirectory();
    this.launchTarget = new LaunchTargetResolver(directory);
    this.setMaxListeners(32);
  }

  /** Default game-server host when no DLL/launch target is available. */
  get defaultServerIp(): string {
    return this.launchTarget.defaultHost;
  }

  cleanStaleTargetFiles(): void {
    this.launchTarget.cleanStaleFiles();
  }

  /** @deprecated Use cleanStaleTargetFiles */
  cleanStalePidFiles(): void {
    this.cleanStaleTargetFiles();
  }

  /** Start the TCP listener. */
  start(host = '127.0.0.1', port = 2050): void {
    Logger.log('Proxy', `Starting listener on ${host}:${port}...`);
    this.listener = net.createServer((socket) => this.onLocalConnect(socket));
    this.listener.listen(port, host, () => {
      Logger.log('Proxy', `Listening on ${host}:${port}`);
      this.emit('listenStarted');
    });
    this.listener.on('error', (err) => {
      Logger.error('Proxy', `Listener error: ${err.message}`, err);
    });
  }

  /** Stop the TCP listener. */
  stop(): void {
    if (!this.listener) return;
    Logger.log('Proxy', 'Stopping listener...');
    this.listener.close();
    this.listener = null;
    this.emit('listenStopped');
  }

  // ─── State Management ─────────────────────────────────────────

  /** Get or create state for a connection by key (GUID from reconnect flow). */
  getState(client: ClientConnection, key: Buffer): State {
    const guid = key.length === 0 ? 'n/a' : key.toString('utf8');

    const newState = new State(client);
    this.states.set(newState.guid, newState);

    Logger.debug('reconnect', 'State', `Lookup — guid from key: "${guid.slice(0, 40)}", states count: ${this.states.size}, found: ${guid !== 'n/a' && this.states.has(guid)}`);

    if (guid !== 'n/a' && this.states.has(guid)) {
      const lastState = this.states.get(guid)!;
      newState.conTargetAddress = lastState.conTargetAddress;
      newState.conTargetPort = lastState.conTargetPort;
      newState.conRealKey = lastState.conRealKey;
      newState.conRealGameId = lastState.conRealGameId;
      newState.conRealKeyTime = lastState.conRealKeyTime;
      newState.pendingKeyRestore = true;
      newState.accessToken = lastState.accessToken;
      if (lastState.helloTemplate) {
        newState.helloTemplate = Buffer.from(lastState.helloTemplate);
        newState.helloKeyOffset = lastState.helloKeyOffset;
      }
      newState.copyStoreFrom(lastState);
      Logger.debug('reconnect', 'State', `Restored from previous — address: ${lastState.conTargetAddress}, port: ${lastState.conTargetPort}, keyLen: ${lastState.conRealKey.length}, gameId: ${lastState.conRealGameId}, keyTime: ${lastState.conRealKeyTime}`);
    }

    return newState;
  }

  // ─── Hook Registration ────────────────────────────────────────

  /**
   * Register a packet handler.
   * @param prepend - if true, handler runs before all other hooks for this packet (safety‑critical, e.g. autonexus).
   */
  hookPacket(packetName: string, handler: PacketHandler, pluginId?: string, prepend = false): void {
    if (!this.packetHooks.has(packetName)) {
      this.packetHooks.set(packetName, []);
    }
    const list = this.packetHooks.get(packetName)!;
    if (prepend) list.unshift(handler);
    else list.push(handler);

    // Track for plugin unloading
    if (pluginId) {
      if (!this.pluginHooks.has(pluginId)) {
        this.pluginHooks.set(pluginId, { packets: new Map(), commands: new Map() });
      }
      const ph = this.pluginHooks.get(pluginId)!;
      if (!ph.packets.has(packetName)) ph.packets.set(packetName, []);
      ph.packets.get(packetName)!.push(handler);
    }
  }

  /** Register a command handler (e.g., /nexus). */
  hookCommand(command: string, handler: CommandHandler, pluginId?: string): void {
    const cmd = command.startsWith('/') ? command.slice(1).toLowerCase() : command.toLowerCase();
    if (!this.commandHooks.has(cmd)) {
      this.commandHooks.set(cmd, []);
    }
    this.commandHooks.get(cmd)!.push(handler);

    if (pluginId) {
      if (!this.pluginHooks.has(pluginId)) {
        this.pluginHooks.set(pluginId, { packets: new Map(), commands: new Map() });
      }
      const ph = this.pluginHooks.get(pluginId)!;
      if (!ph.commands.has(cmd)) ph.commands.set(cmd, []);
      ph.commands.get(cmd)!.push(handler);
    }
  }

  /** Unregister all hooks for a plugin. */
  unhookPlugin(pluginId: string): void {
    const hooks = this.pluginHooks.get(pluginId);
    if (!hooks) return;

    for (const [name, handlers] of hooks.packets) {
      const list = this.packetHooks.get(name);
      if (list) {
        this.packetHooks.set(name, list.filter(h => !handlers.includes(h)));
      }
    }
    for (const [cmd, handlers] of hooks.commands) {
      const list = this.commandHooks.get(cmd);
      if (list) {
        this.commandHooks.set(cmd, list.filter(h => !handlers.includes(h)));
      }
    }

    this.pluginHooks.delete(pluginId);
  }

  // ─── Event Firing ─────────────────────────────────────────────

  /** Fire hooks for a packet from the server. */
  fireServerPacket(client: ClientConnection, packet: Packet): void {
    if (this.listenerCount('serverPacket') > 0) {
      this.emit('serverPacket', client, packet);
    }
    this.firePacketHooks(client, packet);
    // Keep UPDATE free-flowing by default, but preserve plugin rewrites.
    // If a plugin marks the packet modified, ClientConnection will reserialize it
    // before sending to the client instead of forwarding the original raw bytes.
    if (packet.name === 'UPDATE') {
      packet.send = true;
    }
  }

  /** Fire hooks for a packet from the client. */
  fireClientPacket(client: ClientConnection, packet: Packet): void {
    // Check for command interception
    if (packet.name === 'PLAYERTEXT' && packet.isDefined && this.commandHooks.size > 0) {
      const text = (packet.data.text as string).replace('/', '').toLowerCase();
      const parts = text.split(' ');
      const command = parts[0];
      const args = parts.slice(1);

      const handlers = this.commandHooks.get(command);
      if (handlers && handlers.length > 0) {
        let consumed = false;
        for (const handler of handlers) {
          try {
            const result = handler(client, command, args);
            // Legacy handlers return void; treat that as consumed.
            // Handlers can return false to explicitly not consume.
            if (result !== false) consumed = true;
          } catch (err) {
            Logger.error('Proxy', `Command handler error for /${command}`, err as Error);
          }
        }
        if (consumed) {
          packet.send = false; // Consume command only when a handler actively handled it.
        }
      }
    }

    if (this.listenerCount('clientPacket') > 0) {
      this.emit('clientPacket', client, packet);
    }
    this.firePacketHooks(client, packet);
  }

  fireClientConnected(client: ClientConnection): void {
    this.emit('clientConnected', client);
  }

  fireClientDisconnected(client: ClientConnection): void {
    this.emit('clientDisconnected', client);
  }

  private firePacketHooks(client: ClientConnection, packet: Packet): void {
    const handlers = this.packetHooks.get(packet.name);
    if (!handlers || handlers.length === 0) return;

    for (const handler of handlers) {
      try {
        handler(client, packet);
      } catch (err) {
        Logger.error('Proxy', `Packet hook error for ${packet.name}`, err as Error);
      }
    }
  }

  private onLocalConnect(socket: net.Socket): void {
    Logger.log('Proxy', 'Client connected.');
    const client = new ClientConnection(this, socket);
    client.clientId = generateClientId();
    client.originalTargetIp = this.launchTarget.resolveForSocket(socket);
    this.emit('clientBeginConnect', client);
  }
}
