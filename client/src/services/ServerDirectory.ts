import { readFileSync, writeFileSync, existsSync } from 'fs';
import { Logger } from '../util/Logger.js';
import { fetchServerList } from './ServerListFetcher.js';

/**
 * Mutable server name → host (DNS hostname or IP). Shared by proxy, dashboard, and plugins.
 * Refreshed from the RotMG API when an access token is available (e.g. at credential launch).
 */
export class ServerDirectory {
  private servers: Record<string, string> = {};

  loadFromRecord(record: Record<string, string>): void {
    this.servers = { ...record };
  }

  loadFromFile(path: string): boolean {
    try {
      if (!existsSync(path)) return false;
      this.servers = JSON.parse(readFileSync(path, 'utf8'));
      return Object.keys(this.servers).length > 0;
    } catch (err) {
      Logger.warn('ServerDirectory', `Failed to load ${path}: ${(err as Error).message}`);
      return false;
    }
  }

  /** Merge API/fallback entries; existing names are overwritten. */
  merge(record: Record<string, string>): void {
    Object.assign(this.servers, record);
  }

  getHost(serverName: string): string | undefined {
    return this.servers[serverName];
  }

  getAll(): Record<string, string> {
    return { ...this.servers };
  }

  get defaultHost(): string {
    return this.servers.USWest
      || this.servers.USEast
      || this.servers.EUWest
      || Object.values(this.servers)[0]
      || 'uswest3.realmofthemadgod.com';
  }

  /** Fetch live server list and merge (hostnames kept — Node resolves DNS at connect time). */
  async refreshFromApi(accessToken: string): Promise<number> {
    const fresh = await fetchServerList(accessToken);
    const count = Object.keys(fresh).length;
    if (count > 0) {
      this.merge(fresh);
      Logger.log('ServerDirectory', `Refreshed ${count} servers from API`);
    }
    return count;
  }

  /** Optional: persist merged list for offline fallback. */
  saveToFile(path: string): void {
    try {
      writeFileSync(path, JSON.stringify(this.servers, null, 2), 'utf8');
    } catch (err) {
      Logger.warn('ServerDirectory', `Failed to save ${path}: ${(err as Error).message}`);
    }
  }
}

let shared: ServerDirectory | null = null;

export function initServerDirectory(initial?: Record<string, string>): ServerDirectory {
  shared = new ServerDirectory();
  if (initial && Object.keys(initial).length > 0) {
    shared.loadFromRecord(initial);
  }
  return shared;
}

export function getServerDirectory(): ServerDirectory {
  if (!shared) shared = new ServerDirectory();
  return shared;
}

export function pickDefaultServerHost(servers: Record<string, string>): string {
  return servers.USWest
    || servers.USEast
    || servers.EUWest
    || Object.values(servers)[0]
    || 'uswest3.realmofthemadgod.com';
}
