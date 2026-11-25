import { formatHost } from './net'
import { clipboardRangeQuery } from './clipboard-lazy'
import type { SessionType } from './session-type'

export interface DeviceCapabilities {
  screen_capture: boolean
  input_injection: boolean
  clipboard: boolean
  /// The local core can mount remote clipboard files with FUSE and relay
  /// range reads, so copying on the host transfers no bytes.
  clipboard_virtual?: boolean
}

export interface AuthCode {
  mode: 'single' | 'multi'
  value: string
  remaining: number
  uses: number
  long_term_set: boolean
  /// Whether each temporary mode may be used at all.
  single_enabled?: boolean
  multi_enabled?: boolean
}

export interface DeviceInfo {
  device_id: string
  device_name?: string
  avatar?: string
  platform?: string
  token: string
  service: string
  product: string
  version: string
  protocol: number
  host: string
  port: number
  capabilities: DeviceCapabilities
  addresses?: string[]
  code?: AuthCode
}

export interface AccessRequest {
  id: number
  peer_ip: string
  name: string
  client: string
  created_ms: number
}

export interface ServerInfo {
  url: string
  custom_url?: string
  default_url?: string
  using_default?: boolean
  enabled: boolean
  registered: boolean
  online: boolean
  domain?: string
  domain_valid?: boolean
  device_id: string
  display_id: string
  last_error: string
}

export interface ServerConfig extends ServerInfo {
  invite: string
  ca: string
}

export interface DeviceLookup {
  id: string
  display_id: string
  name: string
  online: boolean
  address: string
  peer_ip: string
  port: number
  version: string
  platform?: string
  avatar?: string
}

export interface PeerStatus {
  id: string
  display_id: string
  name: string
  platform?: string
  avatar?: string
  online: boolean
  last_seen_ms: number
}

export interface DomainDevice {
  id: string
  display_id: string
  name: string
  platform?: string
  avatar?: string
  host: string
  port: number
  online: boolean
  last_seen_ms: number
}

export interface DomainConfig {
  domain: string
  domain_valid: boolean
}

export interface DomainList {
  domain: string
  domain_valid: boolean
  devices: DomainDevice[]
}

export interface SavedDevicePreference {
  key: string
  id?: string
  host: string
  port: number
  label: string
  platform?: string
  avatar?: string
  token?: string
  savedAt: number
}

export interface RecentPreference {
  host: string
  port: number
  label: string
  at: number
  token?: string
  id?: string
  platform?: string
  avatar?: string
}

export interface UiPreferences {
  uiMode?: 'simple' | 'advanced'
  /// UI language; mirrored here so the native Windows shell can localize its
  /// tray menu without reading the WebView localStorage.
  language?: string
  savedDevices?: SavedDevicePreference[]
  recents?: RecentPreference[]
  /// Notes shared by device name (normalized name -> note).
  deviceNotes?: Record<string, string>
  /// Last values typed into the simple connect form.
  connectDraft?: { input: string; code: string }
  /// Whether reconnect credentials are persisted at all.
  rememberDevice?: boolean
  /// The remembered first-use answer to the combo-forwarding dialog. Kept in
  /// ui.json so the native shell (whose origin changes every launch) does not
  /// ask again.
  comboForwardAsked?: 'enabled' | 'declined'
  /// Whether the hidden input may use the local IME and send committed text.
  localIme?: boolean
  /// Default picture preset for a new session (viewer-side only).
  defaultPreset?: string
  /// Viewer-owned presets keyed by `molink.preset.<host>:<port>`. Written by
  /// the native shell through ui.json so a restarted viewer keeps each
  /// device's tier even though the core's origin changes every launch.
  presets?: Record<string, { preset: string; fps?: number; updatedAt?: number }>
  /// Advanced画质 mode switch (viewer-global).
  advancedQuality?: boolean
  /// Advanced parameters keyed by `molink.advanced.<host>:<port>`.
  advanced?: Record<
    string,
    {
      fps: number
      quality: number
      bitrateKbps: number
      rateMode: string
      format: string
      chroma: string
      codec: string
    }
  >
  /// Host session tokens keyed by `molink.session.<host>:<port>`. Written by
  /// the native shell through ui.json so a restarted client can reconnect
  /// without spending another verification code.
  sessions?: Record<string, { token: string; savedAt: number }>
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
  device_name?: string
  avatar?: string
  platform?: string
  /// Host desktop session. Absent on an older core, which behaves like
  /// unknown. A Wayland host only reaches XWayland applications.
  session_type?: SessionType
  allow_remote_access?: boolean
  /// This core refuses every outbound session except a loopback
  /// self-connection. Optional so an older core (no field) renders normally.
  host_only?: boolean
  server?: ServerInfo
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
  const securePage = window.location.protocol === 'https:'
  const sameHost =
    host !== null &&
    (host === window.location.hostname || host === '127.0.0.1' ||
     host === 'localhost')
  const scheme = securePage && sameHost ? 'https' : 'http'
  if (host && port) return `${scheme}://${formatHost(host)}:${port}`
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

export interface Av1Device {
  id: string
  name: string
  hardware: boolean
  available: boolean
  detail?: string
}

export interface Av1Effective {
  speed: string
  enc_mode: number
  threads: number
  qp: number
  max_area_percent: number
  hardware?: boolean
}

export interface Av1Encoder {
  id: string
  name: string
  hardware: boolean
  available: boolean
  av1: boolean
  h264: boolean
  vp9: boolean
  vp8: boolean
}

export interface Av1Settings {
  available: boolean
  enabled: boolean
  /// AV1 carries the whole picture; tiles are only a fallback.
  full_frame: boolean
  device: string
  speed: 'auto' | 'fast' | 'balanced' | 'quality'
  qp: number
  max_area_percent: number
  threads: number
  keyframe_interval_seconds: number
  /// Percentage of the screen that must move before the stream takes over.
  stream_enter_percent?: number
  /// "auto" keeps the hardware-first ladder; a pinned codec wins when every
  /// viewer can decode it.
  codec_preference?: string
  /// Video path: stream / tiles (no adaptive mixing).
  video_mode?: string
  devices: Av1Device[]
  /// What this host can actually encode right now. Used by the codec picker
  /// to name an encoder this build/machine does not have.
  encoders?: Av1Encoder[]
  active_quality?: string
  effective?: Av1Effective
}

export interface Av1SettingsUpdate {
  enabled?: boolean
  full_frame?: boolean
  device?: string
  speed?: Av1Settings['speed']
  qp?: number
  max_area_percent?: number
  threads?: number
  keyframe_interval_seconds?: number
  stream_enter_percent?: number
  stream_codec?: string
  video_mode?: string
}

export function fetchAv1Settings(token: string): Promise<Av1Settings> {
  return request<Av1Settings>('/api/encoding', {}, token)
}

export function postAv1Settings(
  token: string,
  update: Av1SettingsUpdate,
): Promise<Av1Settings> {
  const query = new URLSearchParams()
  if (update.enabled !== undefined) {
    query.set('enabled', update.enabled ? '1' : '0')
  }
  if (update.full_frame !== undefined) {
    query.set('full_frame', update.full_frame ? '1' : '0')
  }
  if (update.device !== undefined) query.set('device', update.device)
  if (update.speed !== undefined) query.set('speed', update.speed)
  if (update.qp !== undefined) query.set('qp', String(update.qp))
  if (update.max_area_percent !== undefined) {
    query.set('max_area_percent', String(update.max_area_percent))
  }
  if (update.threads !== undefined) {
    query.set('threads', String(update.threads))
  }
  if (update.keyframe_interval_seconds !== undefined) {
    query.set(
      'keyframe_interval_seconds',
      String(update.keyframe_interval_seconds),
    )
  }
  if (update.stream_enter_percent !== undefined) {
    query.set('stream_enter_percent', String(update.stream_enter_percent))
  }
  if (update.stream_codec !== undefined) {
    query.set('stream_codec', update.stream_codec)
  }
  if (update.video_mode !== undefined) {
    query.set('video_mode', update.video_mode)
  }
  return request<Av1Settings>(
    `/api/encoding?${query.toString()}`,
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
  platform?: string
}

export interface DiscoveryResult {
  enabled: boolean
  devices: DiscoveredPeer[]
}

export function fetchDiscovery(token: string): Promise<DiscoveryResult> {
  return request<DiscoveryResult>('/api/discovery', {}, token)
}

export interface SecuritySettings {
  server_enabled?: boolean
  allow_remote_access: boolean
  allow_web_client: boolean
  discovery_enabled: boolean
  allow_access_requests: boolean
  /// Fan a host clipboard change out to every viewer when no viewer is the
  /// active one. Off by default; the host stores it as
  /// allow_sync_between_sessions.
  allow_sync_between_sessions?: boolean
  encrypt_remote?: boolean
  encrypt_lan?: boolean
  stun_server?: string
  /// Whitespace-separated CIDR list for incoming peers. Empty means no
  /// restriction; loopback is always allowed.
  ip_allowlist?: string
  device_name: string
  long_term_code: string
  /// Host audio send quality: 48 kHz stereo, 24 kHz mono or 16 kHz mono.
  audio_quality?: 'high' | 'balanced' | 'low'
  restart_required: boolean
}

export interface TwoFactorStatus {
  enabled: boolean
  /// `otpauth://` URI for the authenticator app, present while a secret is
  /// waiting to be confirmed.
  uri: string
  issuer: string
  account: string
}

export function fetchTwoFactor(token: string): Promise<TwoFactorStatus> {
  return request<TwoFactorStatus>('/api/2fa', {}, token)
}

/// Confirms the secret with a code from the authenticator app.
export function enableTwoFactor(
  token: string,
  code: string,
): Promise<{ ok: boolean; enabled: boolean }> {
  return request('/api/2fa/enable', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ code }),
  }, token)
}

/// Turning it off needs a current code or the long-term password.
export function disableTwoFactor(
  token: string,
  credential: string,
): Promise<{ ok: boolean }> {
  return request('/api/2fa/disable', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ credential }),
  }, token)
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
    syncBetweenSessions?: boolean
    encryptRemote?: boolean
    encryptLan?: boolean
    name: string
    stunServer?: string
    ipAllowlist?: string
    audioQuality?: 'high' | 'balanced' | 'low'
  },
): Promise<SecuritySettings> {
  const query = new URLSearchParams({
    allow_remote: values.allowRemote ? '1' : '0',
    allow_web: values.allowWeb ? '1' : '0',
    discovery: values.discovery ? '1' : '0',
    access_requests: values.allowAccessRequests ? '1' : '0',
    name: values.name,
  })
  if (values.syncBetweenSessions !== undefined) {
    query.set('sync_between_sessions', values.syncBetweenSessions ? '1' : '0')
  }
  if (values.encryptRemote !== undefined) {
    query.set('encrypt_remote', values.encryptRemote ? '1' : '0')
  }
  if (values.encryptLan !== undefined) {
    query.set('encrypt_lan', values.encryptLan ? '1' : '0')
  }
  if (values.stunServer !== undefined) query.set('stun', values.stunServer)
  if (values.ipAllowlist !== undefined) {
    query.set('ip_allowlist', values.ipAllowlist)
  }
  if (values.audioQuality !== undefined) {
    query.set('audio_quality', values.audioQuality)
  }
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
  platform?: string
  autostart?: boolean
  /// Windows: the ImagePath the service is actually registered with.
  path?: string
  /// Windows: where this build expects the helper binary.
  expected?: string
  /// Windows: `path` is wrong, or the binary it names is missing.
  stale?: boolean
  /// Windows: the core already has administrator rights, so no UAC prompt.
  elevated?: boolean
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

export function postCodePolicy(
  token: string,
  single: boolean,
  multi: boolean,
): Promise<AuthCode> {
  const query = new URLSearchParams({
    single: single ? '1' : '0',
    multi: multi ? '1' : '0',
  })
  return request<AuthCode>(
    `/api/auth/code-policy?${query.toString()}`,
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
  sender_name?: string
  sender_id?: string
  sender_session?: number
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

export interface ClipboardFileInfo {
  id: string
  name: string
  size: number
  /// The local core staged this file from the remote: do not offer it back.
  echo?: boolean
}

export interface ClipboardFilesResult {
  version: number
  files: ClipboardFileInfo[]
}

/// The PNG currently on the *local* core's clipboard (loopback only).
/// `size` is zero when the clipboard holds no image. `echo` is true for an
/// image this core just wrote while applying a remote clipboard.
export interface ClipboardImageInfo {
  version: number
  size: number
  hash?: string
  echo?: boolean
  too_large?: boolean
}

export function fetchClipboardImageInfo(
  token: string,
): Promise<ClipboardImageInfo> {
  return request<ClipboardImageInfo>('/api/clipboard-image-info', {}, token)
}

/// The PNG bytes of the local clipboard image.
export async function fetchClipboardImage(
  token: string,
): Promise<ArrayBuffer> {
  const headers = new Headers()
  if (token) headers.set('Authorization', `Bearer ${token}`)
  if (clientToken) headers.set('X-MoLink-Client', clientToken)
  const response = await fetch(`${coreBaseUrl()}/api/clipboard-image`, {
    headers,
    cache: 'no-store',
  })
  if (!response.ok) {
    let code = ''
    try {
      const body = (await response.json()) as { code?: string }
      code = body.code ?? ''
    } catch {
      // Ignore malformed error bodies.
    }
    throw new ApiError(response.status, code || 'clipboard_image_failed')
  }
  return response.arrayBuffer()
}

/// Put a downloaded PNG on the local clipboard. `text` is the text copied
/// with the same image, if any: both formats are written in one operation so
/// the later one does not clear the earlier one.
export function applyClipboardImage(
  token: string,
  id: string,
  text?: string,
): Promise<{ ok: boolean; count: number }> {
  return request<{ ok: boolean; count: number }>(
    '/api/clipboard-image-apply',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(text ? { ids: [id], text } : { ids: [id] }),
    },
    token,
  )
}

/// Files currently on the *local* core's clipboard (loopback only).
export function fetchClipboardFiles(
  token: string,
): Promise<ClipboardFilesResult> {
  return request<ClipboardFilesResult>('/api/clipboard-files', {}, token)
}

/// Contents of one file on the local clipboard. The id is opaque; the core
/// refuses ids that are no longer in the clipboard.
///
/// With `offset`/`length` only that range is returned, which is how the host's
/// virtual-file `fetch` is answered without reading (or uploading) the whole
/// file.
export async function fetchClipboardFile(
  token: string,
  id: string,
  offset?: number,
  length?: number,
): Promise<ArrayBuffer> {
  const headers = new Headers()
  if (token) headers.set('Authorization', `Bearer ${token}`)
  if (clientToken) headers.set('X-MoLink-Client', clientToken)
  const response = await fetch(
    `${coreBaseUrl()}/api/clipboard-file?id=${encodeURIComponent(id)}` +
      clipboardRangeQuery(offset, length),
    { headers, cache: 'no-store' },
  )
  if (!response.ok) {
    let code = ''
    try {
      const body = (await response.json()) as { code?: string }
      code = body.code ?? ''
    } catch {
      // Ignore malformed error bodies.
    }
    throw new ApiError(response.status, code || 'clipboard_file_failed')
  }
  return response.arrayBuffer()
}

/// One staged remote file. `path` is the final path it will have once applied;
/// the bytes live in a private temp file until then.
export interface StagedClipboardFile {
  id: string
  path: string
}

/// Declare a file that is about to be downloaded into the local staging area.
/// Loopback only: the core refuses a remote page.
export function stageClipboardFile(
  token: string,
  name: string,
  size: number,
  batch?: string,
  /// Path inside the copied folder tree, when the host sent one: the staged
  /// entry then keeps the directory structure instead of flattening it.
  rel?: string,
  isDir?: boolean,
): Promise<StagedClipboardFile> {
  const body: Record<string, unknown> = { name, size }
  if (batch) body.batch = batch
  if (rel) body.rel = rel
  if (isDir) body.is_dir = true
  return request<StagedClipboardFile>(
    '/api/clipboard-stage',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    },
    token,
  )
}

/// Append one contiguous chunk. The core answers 409 when `offset` is not the
/// number of bytes already received and 413 when the chunk crosses the
/// declared size.
export function appendClipboardStage(
  token: string,
  id: string,
  offset: number,
  data: Uint8Array,
): Promise<void> {
  return request<void>(
    `/api/clipboard-stage/${encodeURIComponent(id)}?offset=${offset}`,
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream' },
      // Copy into an ArrayBuffer so fetch does not see a possibly shared
      // backing store (Fetch BodyInit rejects SharedArrayBuffer views).
      body: new Uint8Array(data).buffer,
    },
    token,
  )
}

/// Delete an unfinished staging entry (clipboard changed mid-download).
export function abortClipboardStage(
  token: string,
  id: string,
): Promise<{ ok: boolean }> {
  return request<{ ok: boolean }>(
    `/api/clipboard-stage/${encodeURIComponent(id)}`,
    { method: 'DELETE' },
    token,
  )
}

/// After every file is complete, hand the ordered ids to the local clipboard.
export function applyClipboardStage(
  token: string,
  ids: string[],
): Promise<{ ok: boolean; count: number }> {
  return request<{ ok: boolean; count: number }>(
    '/api/clipboard-apply',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ids }),
    },
    token,
  )
}

export interface VirtualClipboardOfferFile {
  id: number
  name: string
  size: number
}

export interface VirtualClipboardOffer {
  ok: boolean
  mount: string
  omitted: number
  paths: string[]
}

/// Publish remote clipboard files on this machine's clipboard and return the
/// relay generation. Linux mounts them read-only (FUSE); Windows installs an
/// OLE virtual-file data object so Explorer runs the copy itself. Loopback
/// only. The reply's `mount` id is used to open the relay WebSocket; a failed
/// offer throws ApiError (`clipboard_virtual_unavailable`,
/// `clipboard_virtual_mount_failed`, …) so the caller can fall back to
/// downloading.
export function offerVirtualClipboard(
  token: string,
  files: VirtualClipboardOfferFile[],
  session: string,
): Promise<VirtualClipboardOffer> {
  return request<VirtualClipboardOffer>(
    '/api/clipboard-virtual-offer',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ files, session }),
    },
    token,
  )
}

/// Unmount a generation. Without `mount` the core clears whatever it owns;
/// with it, a stale tab cannot unmount a newer copy.
export function clearVirtualClipboard(
  token: string,
  mount?: string,
): Promise<{ ok: boolean; cleared: boolean }> {
  return request<{ ok: boolean; cleared: boolean }>(
    '/api/clipboard-virtual-clear',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(mount ? { mount } : {}),
    },
    token,
  )
}

/// Loopback WebSocket that relays virtual-file pulls. It lives outside
/// `/api/`, so the browser's (header-less) upgrade is not sent through the
/// bearer-token guard; the core checks the peer is loopback instead.
export function virtualClipboardBridgeUrl(mount: string): string {
  const base = coreBaseUrl().replace(/^http/, 'ws')
  return `${base}/ws/clipboard-virtual?mount=${encodeURIComponent(mount)}`
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


export function fetchServerConfig(token: string): Promise<ServerConfig> {
  return request<ServerConfig>('/api/server', {}, token)
}

export function postServerConfig(
  token: string,
  url: string,
  invite: string,
  ca: string,
): Promise<ServerConfig> {
  const query = new URLSearchParams({ url, invite, ca })
  return request<ServerConfig>(
    `/api/server?${query.toString()}`,
    { method: 'POST' },
    token,
  )
}

/// Toggle the remote directory without touching a configured custom URL.
export function postServerEnabled(
  token: string,
  enabled: boolean,
): Promise<ServerConfig> {
  return request<ServerConfig>(
    `/api/server?enabled=${enabled ? 1 : 0}`,
    { method: 'POST' },
    token,
  )
}

export function saveAvatar(token: string, avatar: string): Promise<{ avatar: string }> {
  return request<{ avatar: string }>(
    '/api/avatar',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ avatar }),
    },
    token,
  )
}

export interface FsEntry {
  name: string
  dir: boolean
  size: number
  mtime?: number
  hidden?: boolean
  /// Symlink or Windows junction/reparse point.
  link?: boolean
  /// Present on volume-list rows only: the absolute root to open.
  path?: string
  /// "fixed", "removable", "network", "cdrom", "ram", "home", "unknown".
  kind?: string
  /// Set when the name was readable but its metadata was not.
  error?: string
  root?: boolean
}

export interface FsRoot {
  name: string
  path: string
  kind: string
}

/// A folder listing from the *local* core (`/api/fs/list`). It exists so the
/// desktop client can navigate a path typed by hand, which the browser's File
/// System Access API cannot do. Only valid when `/api/state` reported
/// `local: true`; a remote page must never call it.
export interface FsListing {
  path: string
  root: string
  parent: string
  roots: FsRoot[]
  entries: FsEntry[]
}

export function fetchLocalDirectory(
  token: string,
  path: string,
): Promise<FsListing> {
  const query = new URLSearchParams({ path })
  return request<FsListing>(`/api/fs/list?${query.toString()}`, {}, token)
}

/// `request` with the same auth headers, for endpoints that return bytes or
/// accept a binary body instead of JSON.
function binaryRequest(
  path: string,
  init: RequestInit,
  token: string,
): Promise<Response> {
  const headers = new Headers(init.headers)
  if (token) headers.set('Authorization', `Bearer ${token}`)
  if (clientToken) headers.set('X-MoLink-Client', clientToken)
  return fetch(`${coreBaseUrl()}${path}`, {
    ...init,
    headers,
    cache: 'no-store',
  })
}

/// One range of a local file, read by the local core (desktop shell only).
export async function readLocalFileRange(
  token: string,
  path: string,
  offset: number,
  length: number,
): Promise<Uint8Array> {
  const query = new URLSearchParams({
    path,
    offset: String(offset),
    length: String(length),
  })
  const response = await binaryRequest(
    `/api/fs/read?${query.toString()}`,
    {},
    token,
  )
  if (!response.ok) throw new ApiError(response.status, 'read_failed')
  return new Uint8Array(await response.arrayBuffer())
}

/// Appends one received chunk to a local staging file. `truncate` is set for
/// the first chunk so a retried transfer starts from a clean file.
export async function writeLocalFileChunk(
  token: string,
  path: string,
  offset: number,
  data: Uint8Array,
  truncate: boolean,
): Promise<void> {
  const query = new URLSearchParams({
    path,
    offset: String(offset),
    truncate: truncate ? '1' : '0',
  })
  const response = await binaryRequest(
    `/api/fs/write?${query.toString()}`,
    { method: 'POST', body: data as BodyInit },
    token,
  )
  if (!response.ok) throw new ApiError(response.status, 'write_failed')
}

/// Renames a finished staging file into place. The server picks a non-existing
/// name (`name (1).ext`) rather than overwriting another file.
export async function moveLocalFile(
  token: string,
  from: string,
  to: string,
): Promise<string> {
  const result = await request<{ ok: boolean; path?: string; message?: string }>(
    '/api/fs/move',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ from, to }),
    },
    token,
  )
  if (!result.ok || !result.path) {
    throw new ApiError(500, 'move_failed')
  }
  return result.path
}

/// Creates a folder (and parents) on this machine for a received tree.
export async function makeLocalDirectory(
  token: string,
  path: string,
): Promise<void> {
  const result = await request<{ ok: boolean }>(
    '/api/fs/mkdir',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path }),
    },
    token,
  )
  if (!result.ok) throw new ApiError(500, 'mkdir_failed')
}

/// Removes a staging file after a cancelled/failed download.
export async function removeLocalFile(
  token: string,
  path: string,
): Promise<void> {
  await request<{ ok: boolean }>(
    '/api/fs/remove',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path }),
    },
    token,
  )
}

/// Ask the directory to join a shared code, or to issue a fresh one for this
/// device (which revokes the code it owned).
export function postDomain(
  token: string,
  action: 'join' | 'rotate',
  code?: string,
): Promise<DomainConfig> {
  return request<DomainConfig>(
    '/api/domain',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ action, code }),
    },
    token,
  )
}

export function fetchDomainDevices(token: string): Promise<DomainList> {
  return request<DomainList>('/api/domain/devices', {}, token)
}

export function fetchUiPreferences(token: string): Promise<UiPreferences> {
  return request<UiPreferences>('/api/ui-preferences', {}, token)
}

export function saveUiPreferences(
  token: string,
  value: UiPreferences,
): Promise<UiPreferences> {
  return request<UiPreferences>(
    '/api/ui-preferences',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(value),
    },
    token,
  )
}

/// Batch online status for the saved/recent device list. The directory only
/// returns the caller's own list of IDs; there is no global enumeration.
///
/// Lookups go through the local core: the directory's certificate may be a
/// private/Origin CA that the browser does not trust, while the core carries
/// (or is configured with) that CA.
export async function lookupDevices(
  serverUrl: string,
  token: string,
  ids: string[],
): Promise<PeerStatus[]> {
  if (!serverUrl || ids.length === 0) return []
  const query = ids.slice(0, 20).map(encodeURIComponent).join(',')
  const body = await request<{ devices?: PeerStatus[] }>(
    `/api/directory/devices?ids=${query}`,
    {},
    token,
  )
  return body.devices ?? []
}

/// Resolves a device ID via the local core's directory connection.
export async function lookupDevice(
  serverUrl: string,
  token: string,
  id: string,
): Promise<DeviceLookup> {
  if (!serverUrl) throw new ApiError(0, 'server_not_configured')
  return request<DeviceLookup>(
    `/api/directory/device?id=${encodeURIComponent(id)}`,
    {},
    token,
  )
}
