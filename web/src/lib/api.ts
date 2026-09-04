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

export const api = {
  status: () => request<Status>('/api/status'),
  devices: () => request<{ devices: Device[] }>('/api/devices'),
  scan: (durationMs = 8000) => post('/api/scan', { durationMs }),
  connect: (address: string) => post('/api/connect', { address }),
  disconnect: () => post('/api/disconnect'),
  forgetDevice: () => post('/api/forget-device'),
  refresh: () => post('/api/refresh'),
  setSlot: (slot: number) => post('/api/slot', { slot }),
  testImage: (slot: number, seed?: number, activate = false) =>
    post('/api/test-image', { slot, seed: seed ?? 0, activate }),

  tesserae: () => request<{ clients: TesseraeClient[] }>('/api/tesserae'),
  tesseraeConnect: (opts: {
    slot: number;
    serverUrl: string;
    pairingCode: string;
    deviceId: string;
    token: string;
  }) => post('/api/tesserae/connect', opts),
  tesseraePoll: (slot: number) => post('/api/tesserae/poll', { slot }),
  tesseraeForget: (slot: number) => post('/api/tesserae/forget', { slot }),
  sendSlot: (slot: number) => post('/api/slot/send', { slot }),
  setSlotBadge: (slot: number, on: boolean) =>
    post('/api/slot/badge', { slot, on }),

  async slotImage(slot: number): Promise<Uint8Array> {
    const res = await fetch(`/api/slot/download?slot=${slot}`);
    if (!res.ok) {
      throw new ApiError(`${res.status} ${res.statusText}`);
    }
    return new Uint8Array(await res.arrayBuffer());
  },

  wifi: () => request<WifiStatus>('/api/wifi'),
  wifiScan: () => post('/api/wifi/scan'),
  wifiConnect: (ssid: string, password: string) =>
    post('/api/wifi/connect', { ssid, password }),
  wifiForget: () => post('/api/wifi/forget'),
};
