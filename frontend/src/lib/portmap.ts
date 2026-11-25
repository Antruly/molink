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
