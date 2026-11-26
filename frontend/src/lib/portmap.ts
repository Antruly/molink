/// Pure logic for the controller-side port mapping panel: the preset list,
/// form parsing/validation and the stable error-code -> locale-key mapping.
/// Keeping it here lets `node --test` cover the UI decisions without React.

export interface PortMapping {
  id: number
  listen_host: string
  listen_port: number
  target_host: string
  target_port: number
  enabled: boolean
  listening: boolean
  bytes_in: number
  bytes_out: number
  last_error: string
}

export interface PortmapPreset {
  id: string
  port: number
}

/// The six shortcuts from issue #93. Clicking one only fills the form; the
/// user still presses 添加 so a mapping is never created by a stray click.
export const PORTMAP_PRESETS: readonly PortmapPreset[] = [
  { id: 'rdp', port: 3389 },
  { id: 'ssh', port: 22 },
  { id: 'mysql', port: 3306 },
  { id: 'http', port: 80 },
  { id: 'https', port: 443 },
  { id: 'smb', port: 445 },
]

export interface PortmapForm {
  /// Text field value; empty means "let the system pick" (0 on the wire).
  listenPort: string
  targetHost: string
  targetPort: string
}

export const EMPTY_PORTMAP_FORM: PortmapForm = {
  listenPort: '',
  targetHost: '127.0.0.1',
  targetPort: '',
}

/// Preset click: fill the target port and leave the local port blank so the
/// OS assigns one, matching "点一下填入目标端口，本地端口留空随机".
export function applyPortmapPreset(
  form: PortmapForm,
  preset: PortmapPreset,
): PortmapForm {
  return {
    ...form,
    listenPort: '',
    targetHost: form.targetHost.trim() || '127.0.0.1',
    targetPort: String(preset.port),
  }
}

/// Digits only: 1..65535, or 0 for a blank listen port. NaN/null means the
/// field is not a port at all.
export function parsePortValue(value: string): number | null {
  const text = value.trim()
  if (text === '') return null
  if (!/^\d+$/.test(text)) return null
  const port = Number(text)
  if (!Number.isSafeInteger(port) || port < 1 || port > 65535) return null
  return port
}

export function parseListenPort(value: string): number | null {
  if (value.trim() === '') return 0
  return parsePortValue(value)
}

export function validPortHost(value: string): boolean {
  const host = value.trim()
  if (host === '' || host.length > 255) return false
  // eslint-disable-next-line no-control-regex
  return !/[\s\u0000-\u001f\u007f]/.test(host)
}

export interface PortmapCreateInput {
  listen_host: string
  listen_port: number
  target_host: string
  target_port: number
}

export type PortmapFormError = 'bad_port' | 'bad_target'

export type PortmapFormResult =
  | { ok: true; input: PortmapCreateInput }
  | { ok: false; error: PortmapFormError }

/// Validates the add form. The error names match the backend codes so the
/// panel can reuse the same localized message table.
export function buildPortmapInput(form: PortmapForm): PortmapFormResult {
  const listenPort = parseListenPort(form.listenPort)
  if (listenPort === null) return { ok: false, error: 'bad_port' }
  if (!validPortHost(form.targetHost)) {
    return { ok: false, error: 'bad_target' }
  }
  const targetPort = parsePortValue(form.targetPort)
  if (targetPort === null) return { ok: false, error: 'bad_port' }
  return {
    ok: true,
    input: {
      listen_host: '127.0.0.1',
      listen_port: listenPort,
      target_host: form.targetHost.trim(),
      target_port: targetPort,
    },
  }
}

/// Stable codes returned by /api/portmap and by forward-open-fail. Anything
/// unrecognised (a network failure, an older core) gets a generic message.
export function portmapErrorKey(
  code: string | null | undefined,
): string {
  switch (code) {
    case 'port_forward_disabled':
    case 'bad_port':
    case 'port_in_use':
    case 'not_found':
    case 'bad_target':
    case 'save_failed':
    case 'bad_request':
      return `portmap.errors.${code}`
    default:
      return 'portmap.errors.unknown'
  }
}

/// What the panel should render for a mapping's `last_error`. The core writes
/// short codes; known ones become a sentence (connect_failed names the actual
/// target) while anything unrecognised is shown as-is. `null` means no error.
export interface PortmapLastErrorText {
  /// Locale key, or null when `raw` should be displayed verbatim.
  key: string | null
  values: { target: string }
  raw: string
}

const KNOWN_LAST_ERRORS = new Set([
  'no_session',
  'connect_failed',
  'port_forward_disabled',
  'port_in_use',
])

export function portmapLastErrorText(
  mapping: Pick<PortMapping, 'last_error' | 'target_host' | 'target_port'>,
): PortmapLastErrorText | null {
  const raw = mapping.last_error?.trim() ?? ''
  if (raw === '') return null
  const target = `${mapping.target_host}:${mapping.target_port}`
  return {
    key: KNOWN_LAST_ERRORS.has(raw) ? `portmap.lastError.${raw}` : null,
    values: { target },
    raw,
  }
}

/// The subset of /api/settings the port-map panel needs. The panel only owns
/// the host-side opt-in, so every other field is carried through unchanged
/// when it saves.
export interface PortForwardSecuritySettings {
  allow_remote_access: boolean
  allow_web_client: boolean
  discovery_enabled: boolean
  allow_access_requests: boolean
  device_name: string
  allow_sync_between_sessions?: boolean
  encrypt_remote?: boolean
  encrypt_lan?: boolean
  stun_server?: string
  ip_allowlist?: string
  port_forward_enabled?: boolean
}

/// The value object `postSettings` accepts. Kept here so the switch's submit
/// path is testable without React.
export interface PortForwardSaveValues {
  allowRemote: boolean
  allowWeb: boolean
  discovery: boolean
  allowAccessRequests: boolean
  syncBetweenSessions: boolean
  encryptRemote: boolean
  encryptLan: boolean
  name: string
  stunServer?: string
  ipAllowlist?: string
  portForwardEnabled: boolean
}

/// Missing or older cores default to off, matching the backend.
export function portForwardEnabled(
  settings: PortForwardSecuritySettings | null | undefined,
): boolean {
  return settings?.port_forward_enabled === true
}

/// Builds a full settings save for the host-side toggle. The other values
/// come from the last GET so flipping this switch cannot reset access,
/// transport security, or the device name.
export function portForwardSaveValues(
  settings: PortForwardSecuritySettings,
  enabled: boolean,
): PortForwardSaveValues {
  return {
    allowRemote: settings.allow_remote_access,
    allowWeb: settings.allow_web_client,
    discovery: settings.discovery_enabled,
    allowAccessRequests: settings.allow_access_requests,
    syncBetweenSessions: settings.allow_sync_between_sessions === true,
    encryptRemote: settings.encrypt_remote !== false,
    encryptLan: settings.encrypt_lan === true,
    name: settings.device_name ?? '',
    stunServer: settings.stun_server,
    ipAllowlist: settings.ip_allowlist,
    portForwardEnabled: enabled,
  }
}

export type PortmapStatus = 'listening' | 'error' | 'disabled' | 'stopped'

export function mappingStatus(mapping: PortMapping): PortmapStatus {
  if (mapping.last_error) return 'error'
  if (mapping.listening) return 'listening'
  if (mapping.enabled) return 'stopped'
  return 'disabled'
}

export function formatPortmapBytes(bytes: number): string {
  const value = Number.isFinite(bytes) && bytes > 0 ? bytes : 0
  if (value < 1024) return `${value} B`
  if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KB`
  if (value < 1024 * 1024 * 1024) {
    return `${(value / 1024 / 1024).toFixed(1)} MB`
  }
  return `${(value / 1024 / 1024 / 1024).toFixed(2)} GB`
}
