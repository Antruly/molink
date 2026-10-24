/// One input box accepts a 9-character device ID or an address. Addresses may
/// omit the port; `host:port` and `[IPv6]:port` are also accepted.
export interface ParsedTarget {
  kind: 'id' | 'address'
  id?: string
  host: string
  port: number
  label: string
}

export function normalizeDeviceId(raw: string): string {
  return raw
    .replace(/[\s\-_]/g, '')
    .toUpperCase()
    .replace(/[^A-Z0-9]/g, '')
}

export function defaultSessionPort(): number {
  const query = new URLSearchParams(window.location.search).get('port')
  const parsed = query ? Number(query) : NaN
  return Number.isFinite(parsed) && parsed > 0 && parsed <= 65535
    ? parsed
    : 28418
}

export function parseTarget(raw: string): ParsedTarget | null {
  const value = raw.trim()
  if (!value) return null
  const defaultPort = defaultSessionPort()
  if (!value.includes('.') && !value.includes(':') && !value.includes('/')) {
    const id = normalizeDeviceId(value)
    if (id.length === 9) {
      return { kind: 'id', id, host: '', port: defaultPort, label: id }
    }
  }
  if (value.startsWith('[')) {
    const close = value.indexOf(']')
    if (close <= 1) return null
    const host = value.slice(1, close)
    const rest = value.slice(close + 1)
    if (rest === '') return { kind: 'address', host, port: defaultPort, label: host }
    if (!rest.startsWith(':')) return null
    const port = Number(rest.slice(1))
    if (!Number.isFinite(port) || port <= 0 || port > 65535) return null
    return { kind: 'address', host, port, label: host }
  }
  const colon = value.lastIndexOf(':')
  if (colon > 0 && value.indexOf(':') === colon) {
    const host = value.slice(0, colon)
    const port = Number(value.slice(colon + 1))
    if (!host || !Number.isFinite(port) || port <= 0 || port > 65535) return null
    return { kind: 'address', host, port, label: host }
  }
  if (value.includes(':')) {
    // Bare IPv6 without a port.
    return { kind: 'address', host: value, port: defaultPort, label: value }
  }
  return { kind: 'address', host: value, port: defaultPort, label: value }
}
