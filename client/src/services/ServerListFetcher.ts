import dns from 'dns/promises';
import { Logger } from '../util/Logger.js';

const API_URL = 'https://www.realmofthemadgod.com/account/servers';
const IPV4_REGEX = /^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$/;

export type FetchServerListOptions = {
  /** When true, resolve DNS hostnames to IPv4 before returning. Default false — prefer live DNS at connect time. */
  resolveDns?: boolean;
};

/**
 * Fetches the live server list from the RotMG API.
 * Returns server name → host (DNS hostname from API, or IP if resolveDns is true).
 */
export async function fetchServerList(
  accessToken: string,
  options: FetchServerListOptions = {},
): Promise<Record<string, string>> {
  const res = await fetch(API_URL, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/x-www-form-urlencoded',
      'User-Agent': 'UnityPlayer/2021.3.31f1 (UnityWebRequest/1.0, libcurl/8.5.0-DEV)',
      'X-Unity-Version': '2021.3.31f1',
    },
    body: new URLSearchParams({
      accessToken,
      game_net: 'Unity',
      play_platform: 'Unity',
      game_net_user_id: '',
    }),
  });

  if (!res.ok) {
    throw new Error(`Server list API returned ${res.status}`);
  }

  const xml = await res.text();
  const parsed = parseServerXml(xml);
  if (options.resolveDns) {
    return resolveServerHosts(parsed);
  }
  return parsed;
}

function parseServerXml(xml: string): Record<string, string> {
  const servers: Record<string, string> = {};

  const serverBlocks = xml.match(/<Server>[\s\S]*?<\/Server>/g);
  if (!serverBlocks) return servers;

  for (const block of serverBlocks) {
    if (block.includes('<AdminOnly/>') || block.includes('<AdminOnly>')) continue;

    const nameMatch = block.match(/<Name>([^<]+)<\/Name>/);
    const dnsMatch = block.match(/<DNS>([^<]+)<\/DNS>/);

    if (nameMatch && dnsMatch) {
      servers[nameMatch[1]] = dnsMatch[1].trim();
    }
  }

  return servers;
}

async function resolveServerHosts(servers: Record<string, string>): Promise<Record<string, string>> {
  const out: Record<string, string> = {};
  await Promise.all(
    Object.entries(servers).map(async ([name, host]) => {
      out[name] = await resolveHost(host);
    }),
  );
  return out;
}

async function resolveHost(host: string): Promise<string> {
  const trimmed = host.trim();
  if (!trimmed || IPV4_REGEX.test(trimmed)) return trimmed;
  try {
    const { address } = await dns.lookup(trimmed);
    return address;
  } catch (err) {
    Logger.warn('ServerListFetcher', `DNS lookup failed for ${trimmed}: ${(err as Error).message}`);
    return trimmed;
  }
}
