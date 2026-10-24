export class ApiError extends Error {
  readonly status: number
  readonly code: string
  readonly retryAfter: number

  constructor(status: number, code: string, message: string, retryAfter = 0) {
    super(message)
    this.status = status
    this.code = code
    this.retryAfter = retryAfter
  }
}

export interface User {
  id: string
  username: string
  display_name: string
  role: 'admin' | 'operator' | 'viewer'
  disabled: boolean
  must_change_password: boolean
  created_ms: number
  last_login_ms: number
}

export interface SessionInfo {
  authenticated: boolean
  needs_setup: boolean
  version?: string
  user?: User
  csrf_token?: string
  expires_at_ms?: number
}

export interface Overview {
  version: string
  uptime_ms: number
  started_at_ms: number
  device_api: { host: string; port: number; tls: boolean }
  admin_api: { host: string; port: number; tls: boolean }
  devices: { total: number; online: number; offline: number }
  users: { total: number }
  security: { active_bans: number }
  database: { driver: string; schema_version: number }
  relay: { supported: boolean }
}

export interface Device {
  id: string
  display_id: string
  name: string
  address: string
  peer_ip: string
  port: number
  version: string
  online: boolean
  created_ms: number
  last_seen_ms: number
}

export interface DevicePage {
  total: number
  page: number
  page_size: number
  items: Device[]
}

export interface Ban {
  id: string
  target: string
  kind: string
  reason: string
  source: string
  created_ms: number
  expires_ms: number
  created_by: string
  note: string
}

export interface BanPage {
  total: number
  items: Ban[]
}

export interface AuditEvent {
  id: number
  ts_ms: number
  event: string
  result: string
  actor_id: string
  actor_name: string
  role: string
  ip: string
  target: string
  details: string
}

export interface AuditPage {
  total: number
  items: AuditEvent[]
}

export interface SecuritySettings {
  ip_policy: {
    allow_private: boolean
    allow_mainland_china: boolean
    allow_overseas: boolean
    custom_allow: string[]
    custom_deny: string[]
    apply_to_admin: boolean
  }
  trusted_proxies: string[]
  limits: Record<string, number>
  login: {
    failure_limit: number
    failure_window_seconds: number
    ban_ladder_seconds: number[]
    strike_reset_seconds: number
    throttle_loopback: boolean
  }
  session: {
    idle_seconds: number
    absolute_seconds: number
    secure_cookie: string
    trust_forwarded_proto: boolean
  }
  password: { min_length: number; max_length: number }
}

export interface ServerSettings {
  name: string
  public_url: string
  registration_token: string
  offline_after_seconds: number
  max_devices: number
}

export interface AuthResult {
  user: User
  csrf_token: string
  expires_at_ms: number
}

function csrfCookie(): string {
  const prefix = 'molink_admin_csrf='
  for (const part of document.cookie.split(';')) {
    const value = part.trim()
    if (value.startsWith(prefix)) {
      return decodeURIComponent(value.slice(prefix.length))
    }
  }
  return ''
}

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  const method = (init.method ?? 'GET').toUpperCase()
  const headers = new Headers(init.headers)
  if (init.body !== undefined) headers.set('Content-Type', 'application/json')
  if (method !== 'GET' && method !== 'HEAD') {
    const token = csrfCookie()
    if (token) headers.set('X-CSRF-Token', token)
  }
  const response = await fetch(path, {
    credentials: 'same-origin',
    cache: 'no-store',
    ...init,
    method,
    headers,
  })
  if (response.status === 204) return undefined as T
  const text = await response.text()
  let body: unknown = null
  if (text) {
    try {
      body = JSON.parse(text)
    } catch {
      body = null
    }
  }
  if (!response.ok) {
    const record = (body ?? {}) as { code?: string; message?: string; retry_after?: number }
    const retry = Number(response.headers.get('Retry-After') ?? record.retry_after ?? 0)
    throw new ApiError(
      response.status,
      record.code ?? `http_${response.status}`,
      record.message ?? `请求失败（HTTP ${response.status}）`,
      Number.isFinite(retry) ? retry : 0,
    )
  }
  return body as T
}

export const api = {
  session: () => request<SessionInfo>('/api/admin/session'),
  setup: (body: {
    username: string
    display_name: string
    password: string
    setup_token?: string
  }) => request<AuthResult>('/api/admin/setup', { method: 'POST', body: JSON.stringify(body) }),
  login: (username: string, password: string) =>
    request<AuthResult>('/api/admin/login', {
      method: 'POST',
      body: JSON.stringify({ username, password }),
    }),
  logout: () => request<void>('/api/admin/logout', { method: 'POST' }),
  overview: () => request<Overview>('/api/admin/overview'),
  devices: (params: URLSearchParams) => request<DevicePage>(`/api/admin/devices?${params}`),
  revokeDevice: (id: string) =>
    request<void>(`/api/admin/devices/${encodeURIComponent(id)}/revoke`, { method: 'POST' }),
  users: () => request<{ items: User[] }>('/api/admin/users'),
  createUser: (body: {
    username: string
    display_name: string
    role: string
    password: string
  }) => request<User>('/api/admin/users', { method: 'POST', body: JSON.stringify(body) }),
  updateUser: (
    id: string,
    body: { display_name: string; role: string; disabled: boolean },
  ) =>
    request<void>(`/api/admin/users/${encodeURIComponent(id)}`, {
      method: 'PATCH',
      body: JSON.stringify(body),
    }),
  deleteUser: (id: string) =>
    request<void>(`/api/admin/users/${encodeURIComponent(id)}`, { method: 'DELETE' }),
  resetPassword: (id: string, password: string) =>
    request<void>(`/api/admin/users/${encodeURIComponent(id)}/password`, {
      method: 'POST',
      body: JSON.stringify({ new_password: password }),
    }),
  changePassword: (currentPassword: string, newPassword: string) =>
    request<void>('/api/admin/me/password', {
      method: 'POST',
      body: JSON.stringify({
        current_password: currentPassword,
        new_password: newPassword,
      }),
    }),
  security: () => request<SecuritySettings>('/api/admin/security'),
  saveSecurity: (value: SecuritySettings) =>
    request<SecuritySettings>('/api/admin/security', {
      method: 'PUT',
      body: JSON.stringify(value),
    }),
  serverSettings: () => request<ServerSettings>('/api/admin/settings/server'),
  saveServerSettings: (value: ServerSettings) =>
    request<ServerSettings>('/api/admin/settings/server', {
      method: 'PUT',
      body: JSON.stringify(value),
    }),
  bans: (params: URLSearchParams) => request<BanPage>(`/api/admin/bans?${params}`),
  addBan: (body: {
    target: string
    reason: string
    duration_seconds: number
    note: string
  }) => request<{ id: string }>('/api/admin/bans', { method: 'POST', body: JSON.stringify(body) }),
  deleteBan: (id: string) =>
    request<void>(`/api/admin/bans/${encodeURIComponent(id)}`, { method: 'DELETE' }),
  audit: (params: URLSearchParams) => request<AuditPage>(`/api/admin/audit?${params}`),
}
