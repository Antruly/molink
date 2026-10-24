import { formatHost } from './net'

export interface DeviceCapabilities {
  screen_capture: boolean
  input_injection: boolean
  clipboard: boolean
}

export interface AuthCode {
  mode: 'single' | 'multi'
  value: string
  remaining: number
  uses: number
  long_term_set: boolean
}

export interface DeviceInfo {
  device_id: string
  device_name?: string
  token: string
  service: string
  product: string
  version: string
  protocol: number
  host: string
  port: number
  capabilities: DeviceCapabilities
  code?: AuthCode
}

export interface AccessRequest {
  id: number
  peer_ip: string
  name: string
  client: string
  created_ms: number
}

export interface ServerState {
  service: string
  product: string
  version: string
  protocol: number
  device_id: string
  local: boolean
  locked: boolean
  allow_access_requests: boolean
  access_requests?: AccessRequest[]
  lock_timeout: number
  open_sessions: number
  host: string
  port: number
  bound_port: number
  tcp_port: number
  udp_port: number
  restart_required: boolean
}

export interface LoginResult {
  ok: boolean
  session_token: string
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
/// Per-launch token injected by the desktop shell. The core requires it for
/// loopback UI/state/device access when allow_web_client is off.
const clientToken = params.get('client') ?? ''

/// Base URL of the local core. Three cases: shell/Vite pass host+port in the
/// query, the page itself is served by the core, or we fall back to the
/// default port.
export function coreBaseUrl(): string {
  const host = params.get('host')
  const port = params.get('port')
  if (host && port) return `http://${formatHost(host)}:${port}`
  if (window.location.port && window.location.port !== '1420') {
    return window.location.origin
  }
  const fallbackPort = port ?? '28418'
  return `http://127.0.0.1:${fallbackPort}`
}

async function request<T>(
  path: string,
  init: RequestInit = {},
  token?: string,
): Promise<T> {
  const headers = new Headers(init.headers)
  if (token) headers.set('Authorization', `Bearer ${token}`)
  if (clientToken) headers.set('X-MoLink-Client', clientToken)
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

export interface NetworkPorts {
  tcp_port: number
  udp_port: number
  bound_port: number
  restart_required: boolean
}

export function postNetworkPorts(
  token: string,
  tcp: number,
  udp: number,
): Promise<NetworkPorts> {
  return request<NetworkPorts>(
    `/api/network?tcp=${tcp}&udp=${udp}`,
    { method: 'POST' },
    token,
  )
}

export interface DiscoveredPeer {
  device_id: string
  name: string
  host: string
  port: number
  last_seen_ms: number
}

export interface DiscoveryResult {
  enabled: boolean
  devices: DiscoveredPeer[]
}

export function fetchDiscovery(token: string): Promise<DiscoveryResult> {
  return request<DiscoveryResult>('/api/discovery', {}, token)
}

export interface SecuritySettings {
  allow_remote_access: boolean
  allow_web_client: boolean
  discovery_enabled: boolean
  allow_access_requests: boolean
  device_name: string
  long_term_code: string
  restart_required: boolean
}

export function fetchSettings(token: string): Promise<SecuritySettings> {
  return request<SecuritySettings>('/api/settings', {}, token)
}

export function postSettings(
  token: string,
  values: {
    allowRemote: boolean
    allowWeb: boolean
    discovery: boolean
    allowAccessRequests: boolean
    name: string
  },
): Promise<SecuritySettings> {
  const query = new URLSearchParams({
    allow_remote: values.allowRemote ? '1' : '0',
    allow_web: values.allowWeb ? '1' : '0',
    discovery: values.discovery ? '1' : '0',
    access_requests: values.allowAccessRequests ? '1' : '0',
    name: values.name,
  })
  return request<SecuritySettings>(
    `/api/settings?${query.toString()}`,
    { method: 'POST' },
    token,
  )
}

export interface WindowsSession {
  id: number
  state: number
  station: string
  user: string
}

export interface WindowsSessionsResult {
  ok: boolean
  available?: boolean
  error?: string
  sessions?: WindowsSession[]
}

export function fetchWindowsSessions(
  token: string,
): Promise<WindowsSessionsResult> {
  return request<WindowsSessionsResult>('/api/windows/sessions', {}, token)
}

export function postWindowsSession(
  token: string,
  action: 'switch' | 'kick',
  id: number,
): Promise<{ ok: boolean; error?: string }> {
  return request<{ ok: boolean; error?: string }>(
    `/api/windows/session/${action}?id=${id}`,
    { method: 'POST' },
    token,
  )
}

export interface ServiceStatus {
  available: boolean
  installed: boolean
  running: boolean
  desired: boolean
  error?: string
}

export function fetchServiceStatus(token: string): Promise<ServiceStatus> {
  return request<ServiceStatus>('/api/service/status', {}, token)
}

export function postServiceAction(
  token: string,
  action: 'install' | 'uninstall' | 'start' | 'stop' | 'restart',
): Promise<{ ok: boolean; error?: string }> {
  return request<{ ok: boolean; error?: string }>(
    `/api/service/${action}`,
    { method: 'POST' },
    token,
  )
}

export function postAccessDecision(
  token: string,
  id: number,
  allow: boolean,
): Promise<{ ok: boolean }> {
  return request<{ ok: boolean }>(
    `/api/access/${allow ? 'approve' : 'deny'}?id=${id}`,
    { method: 'POST' },
    token,
  )
}

export function postAuthCode(
  token: string,
  mode: 'single' | 'multi',
  uses: number,
  value: string,
): Promise<AuthCode> {
  const query = new URLSearchParams({ mode, uses: String(uses), value })
  return request<AuthCode>(
    `/api/auth/code?${query.toString()}`,
    { method: 'POST' },
    token,
  )
}

export async function postLongTermCode(
  token: string,
  value: string,
): Promise<string> {
  const query = new URLSearchParams({ value })
  const result = await request<{ long_term_code: string }>(
    `/api/auth/long-term?${query.toString()}`,
    { method: 'POST' },
    token,
  )
  return result.long_term_code
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

export interface ChatEntry {
  id: number
  from: 'remote' | 'host' | string
  text: string
  ts: number
}

export interface ChatResult {
  messages: ChatEntry[]
  cursor: number
}

/// Host-side chat log. Loopback only; the session websocket carries the other
/// direction for a controlling client.
export function fetchChat(token: string, since: number): Promise<ChatResult> {
  return request<ChatResult>(`/api/chat?since=${since}`, {}, token)
}

export function postChat(token: string, text: string): Promise<void> {
  const query = new URLSearchParams({ text })
  return request<void>(`/api/chat?${query.toString()}`, { method: 'POST' }, token)
}

export interface CallState {
  state: 'idle' | 'ringing' | 'active'
  started_ms: number
  host_microphone: boolean
}

export function fetchCall(token: string): Promise<CallState> {
  return request<CallState>('/api/call', {}, token)
}

export function postCall(
  token: string,
  action: 'accept' | 'decline' | 'end',
): Promise<void> {
  return request<void>(
    `/api/call?action=${action}`,
    { method: 'POST' },
    token,
  )
}
