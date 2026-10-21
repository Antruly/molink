export interface DeviceCapabilities {
  screen_capture: boolean
  input_injection: boolean
  clipboard: boolean
}

export interface DeviceInfo {
  device_id: string
  token: string
  service: string
  product: string
  version: string
  protocol: number
  host: string
  port: number
  capabilities: DeviceCapabilities
}

export interface ServerState {
  service: string
  product: string
  version: string
  protocol: number
  device_id: string
  local: boolean
  locked: boolean
  lock_timeout: number
  open_sessions: number
  host: string
  port: number
}

export interface LoginResult {
  ok: boolean
  device_id: string
  service: string
  version: string
  protocol: number
  local: boolean
  lock_timeout: number
}

export interface CoreInfo {
  device_id?: string
  service: string
  product: string
  version: string
  protocol: number
  capabilities: DeviceCapabilities
  sessions: number
}

export class ApiError extends Error {
  readonly status: number
  readonly code: string

  constructor(status: number, code: string) {
    super(code)
    this.status = status
    this.code = code
  }
}

const params = new URLSearchParams(window.location.search)

/// Base URL of the local core. Three cases: shell/Vite pass host+port in the
/// query, the page itself is served by the core, or we fall back to the
/// default port.
export function coreBaseUrl(): string {
  const host = params.get('host')
  const port = params.get('port')
  if (host && port) return `http://${host}:${port}`
  if (window.location.port && window.location.port !== '1420') {
    return window.location.origin
  }
  const fallbackPort = port ?? '21118'
  return `http://127.0.0.1:${fallbackPort}`
}

async function request<T>(
  path: string,
  init: RequestInit = {},
  token?: string,
): Promise<T> {
  const headers = new Headers(init.headers)
  if (token) headers.set('Authorization', `Bearer ${token}`)
  const response = await fetch(`${coreBaseUrl()}${path}`, {
    ...init,
    headers,
    cache: 'no-store',
  })
  if (!response.ok) {
    let code = ''
    try {
      const body = (await response.json()) as { code?: string; error?: string }
      code = body.code ?? body.error ?? ''
    } catch {
      code = ''
    }
    throw new ApiError(response.status, code || `http_${response.status}`)
  }
  if (response.status === 204) return undefined as T
  return (await response.json()) as T
}

export function fetchState(): Promise<ServerState> {
  return request<ServerState>('/api/state')
}

export async function fetchDeviceInfo(): Promise<DeviceInfo> {
  return request<DeviceInfo>('/api/device')
}

export function login(token: string): Promise<LoginResult> {
  return request<LoginResult>('/api/login', { method: 'POST' }, token)
}

export function fetchInfo(token: string): Promise<CoreInfo> {
  return request<CoreInfo>('/api/info', {}, token)
}

export function postActivity(token: string): Promise<void> {
  return request<void>('/api/activity', { method: 'POST' }, token)
}

export function postUiState(
  token: string,
  client: string,
  sessions: number,
): Promise<void> {
  const query = new URLSearchParams({
    client,
    sessions: String(sessions),
  })
  return request<void>(
    `/api/ui-state?${query.toString()}`,
    { method: 'POST' },
    token,
  )
}

export async function postLockTimeout(
  token: string,
  seconds: number,
): Promise<number> {
  const result = await request<{ lock_timeout: number }>(
    `/api/lock-timeout?seconds=${seconds}`,
    { method: 'POST' },
    token,
  )
  return result.lock_timeout
}
