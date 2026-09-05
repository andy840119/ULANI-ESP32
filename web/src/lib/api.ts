/* Thin wrapper over the firmware REST API. */

export type UlaniState =
  | 'off'
  | 'idle'
  | 'scanning'
  | 'connecting'
  | 'discovering'
  | 'ready'
  | 'transferring';

export interface Status {
  state: UlaniState;
  connected: boolean;
  activeSlot: number;
  batteryRaw: number;
  /* Absent until a reading has been taken. See docs/protocol.md on the scale. */
  batteryLevel?: number;
  /* How long ago each reading came off the calendar. Absent means never. */
  batteryAgeMs?: number;
  slotAgeMs?: number;
  address: string;
  name: string;
  error: string;
  transfer: { active: boolean; slot: number; attempt: number; sent: number; total: number };
  lastTransfer?: { ok: boolean; rsp: number };
  /* Scan results ride along with the status so the UI needs only one poll. */
  devices: Device[];
  /* The device reconnected to on power-up. Absent if none is remembered. */
  savedDevice?: { address: string; name: string; autoConnect: boolean };
  /* How long the link is held before the board hands the calendar back. 0 = never. */
  idleTimeoutMs: number;
}

export interface Device {
  name: string;
  address: string;
  rssi: number;
}

export type WifiState = 'disabled' | 'connecting' | 'connected' | 'failed';

export interface WifiNetwork {
  ssid: string;
  rssi: number;
  channel: number;
  open: boolean;
}

export interface WifiStatus {
  state: WifiState;
  ssid: string;
  ip: string;
  rssi: number;
  lastReason: number;
  scanning: boolean;
  networks: WifiNetwork[];
}

export type TesseraeState = 'disabled' | 'unregistered' | 'idle' | 'working' | 'error';

export interface TesseraeClient {
  slot: number;
  state: TesseraeState;
  serverUrl: string;
  deviceId: string;
  registered: boolean;
  nextPollS: number;
  secondsUntilPoll: number;
  /* Unix seconds, from the server's clock. 0 = unknown / not yet. */
  lastCheckEpoch: number;
  lastFrameEpoch: number;
  lastSentEpoch: number;
  /* A frame is stored for this slot and can be previewed. */
  hasFrame: boolean;
  /* Stamp the page number in the corner before sending this page. */
  badge: boolean;
  error: string;
}

class ApiError extends Error {}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(path, {
    ...init,
    headers: init?.body ? { 'Content-Type': 'application/json' } : undefined,
  });
  const body = await res.json().catch(() => null);
  if (!res.ok) {
    throw new ApiError(body?.error ?? `${res.status} ${res.statusText}`);
  }
  return body as T;
}

const post = (path: string, payload?: unknown) =>
  request<{ ok: boolean }>(path, {
    method: 'POST',
    body: payload === undefined ? undefined : JSON.stringify(payload),
  });

/*
 * Grouped by subsystem, matching the /api/<group>/... paths on the firmware:
 * system (the board), calendar (the ULANI device + its four pages), wifi, and
 * tesserae (the render service). Reads are get(); everything else is a verb.
 */
export const api = {
  /* The board itself: firmware version and persisted settings. */
  system: {
    version: () =>
      request<{ version: string; production: boolean }>('/api/system/version'),
    /* How long to hold the calendar link before dropping it to save power. A
     * board-wide preference, so it lives here rather than on api.calendar. */
    setCalendarKeepAlive: (ms: number) =>
      post('/api/system/settings', { idleTimeoutMs: ms }),
    exportUrl: '/api/system/settings/export',
    import: (json: string) =>
      request<{ ok: boolean; restored: number; rebooting: boolean }>(
        '/api/system/settings/import',
        { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: json },
      ),
  },

  /* The ULANI calendar over BLE, and its four pages (slots). */
  calendar: {
    get: () => request<Status>('/api/calendar/status'),
    devices: () => request<{ devices: Device[] }>('/api/calendar/devices'),
    scan: (durationMs = 8000) => post('/api/calendar/scan', { durationMs }),
    connect: (address: string) => post('/api/calendar/connect', { address }),
    disconnect: () => post('/api/calendar/disconnect'),
    forget: () => post('/api/calendar/forget'),
    refresh: () => post('/api/calendar/refresh'),
    setPage: (slot: number) => post('/api/calendar/slot', { slot }),
    testImage: (slot: number, seed?: number, activate = false) =>
      post('/api/calendar/test-image', { slot, seed: seed ?? 0, activate }),
    sendPage: (slot: number) => post('/api/calendar/slot/send', { slot }),
    setBadge: (slot: number, on: boolean) =>
      post('/api/calendar/slot/badge', { slot, on }),
    async pageImage(slot: number): Promise<Uint8Array> {
      const res = await fetch(`/api/calendar/slot/download?slot=${slot}`);
      if (!res.ok) {
        throw new ApiError(`${res.status} ${res.statusText}`);
      }
      return new Uint8Array(await res.arrayBuffer());
    },
  },

  wifi: {
    get: () => request<WifiStatus>('/api/wifi/status'),
    scan: () => post('/api/wifi/scan'),
    connect: (ssid: string, password: string) =>
      post('/api/wifi/connect', { ssid, password }),
    forget: () => post('/api/wifi/forget'),
  },

  /* The self-hosted Tesserae render service, one client per page. */
  tesserae: {
    get: () => request<{ clients: TesseraeClient[] }>('/api/tesserae/status'),
    connect: (opts: {
      slot: number;
      serverUrl: string;
      pairingCode: string;
      deviceId: string;
      token: string;
    }) => post('/api/tesserae/connect', opts),
    poll: (slot: number) => post('/api/tesserae/poll', { slot }),
    forget: (slot: number) => post('/api/tesserae/forget', { slot }),
  },
};
