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
  address: string;
  name: string;
  error: string;
  transfer: { active: boolean; slot: number; sent: number; total: number };
  lastTransfer?: { ok: boolean; rsp: number };
  /* Scan results ride along with the status so the UI needs only one poll. */
  devices: Device[];
}

export interface Device {
  name: string;
  address: string;
  rssi: number;
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
  refresh: () => post('/api/refresh'),
  setSlot: (slot: number) => post('/api/slot', { slot }),
  testImage: (slot: number, seed?: number) =>
    post('/api/test-image', { slot, seed: seed ?? 0 }),
};
