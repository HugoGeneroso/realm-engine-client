// Pure char-list / dashboard-overview parsing helpers, extracted from DevServer.
// No instance state (`this`) — safe standalone functions.

export interface DashboardAccountEquipmentToken {
  objectType: number;
  uniqueId: string | null;
}

export function formatObjectTypeHex(objectType: number): string {
  const safeType = Number.isFinite(objectType) ? Math.max(0, Math.trunc(objectType)) : 0;
  return `0x${safeType.toString(16)}`;
}

export function parseCharListNumber(raw: unknown): number {
  const value = Number(raw);
  return Number.isFinite(value) ? value : 0;
}

export function parseCharListBoolean(raw: unknown): boolean {
  const value = String(raw ?? '').trim().toLowerCase();
  return value === '1' || value === 'true';
}

export function parseCharListObjectTypes(raw: unknown, minimumLength = 0): number[] {
  const parsed = String(raw ?? '')
    .split(',')
    .map((value) => {
      const n = Number.parseInt(String(value ?? '').trim(), 10);
      return Number.isFinite(n) ? n : -1;
    });
  while (parsed.length < minimumLength) parsed.push(-1);
  return parsed;
}

export function parseDashboardEquipmentTokens(raw: unknown, minimumLength = 0): DashboardAccountEquipmentToken[] {
  const parsed = String(raw ?? '')
    .split(',')
    .map((value) => String(value ?? '').trim())
    .filter(Boolean)
    .map((value) => {
      const hashIndex = value.indexOf('#');
      const objectTypeText = hashIndex >= 0 ? value.slice(0, hashIndex).trim() : value;
      const uniqueIdText = hashIndex >= 0 ? value.slice(hashIndex + 1).trim() : '';
      const objectType = Number.parseInt(objectTypeText, 10);
      return {
        objectType: Number.isFinite(objectType) ? objectType : -1,
        uniqueId: uniqueIdText || null,
      } satisfies DashboardAccountEquipmentToken;
    });
  while (parsed.length < minimumLength) {
    parsed.push({ objectType: -1, uniqueId: null });
  }
  return parsed;
}

export function buildDashboardUniqueItemLookup(rawUniqueItemInfo: unknown): Map<string, string[]> {
  const lookup = new Map<string, string[]>();
  const uniqueNode = rawUniqueItemInfo && typeof rawUniqueItemInfo === 'object'
    ? rawUniqueItemInfo as Record<string, unknown>
    : null;
  const rawItemData = uniqueNode?.ItemData;
  const entries = Array.isArray(rawItemData) ? rawItemData : (rawItemData ? [rawItemData] : []);
  for (const entry of entries) {
    if (!entry || typeof entry !== 'object') continue;
    const node = entry as Record<string, unknown>;
    const objectType = Number.parseInt(String(node['@_type'] ?? '').trim(), 10);
    if (!Number.isFinite(objectType)) continue;
    const uniqueId = String(node['@_id'] ?? '').trim();
    const encoded = String(node['#text'] ?? '').trim();
    if (!encoded) continue;
    const key = `${objectType}#${uniqueId}`;
    const bucket = lookup.get(key);
    if (bucket) bucket.push(encoded);
    else lookup.set(key, [encoded]);
  }
  return lookup;
}

export function decodeDashboardEnchantIds(code: string | null | undefined): number[] {
  const rawCode = String(code || '').trim();
  if (!rawCode) return [];
  try {
    const normalized = rawCode
      .replace(/-/g, '+')
      .replace(/_/g, '/')
      .padEnd(Math.ceil(rawCode.length / 4) * 4, '=');
    const bytes = Buffer.from(normalized, 'base64');
    if (bytes.length <= 3) return [];
    const enchantIds: number[] = [];
    for (let pos = 3; pos + 1 < bytes.length; pos += 2) {
      const value = bytes.readUInt16LE(pos);
      if (value === 0xfffd) break;
      enchantIds.push(value === 0xfffe ? 0 : value);
    }
    return enchantIds;
  } catch {
    return [];
  }
}
