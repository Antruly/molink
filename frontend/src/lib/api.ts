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

export async function fetchDeviceInfo(): Promise<DeviceInfo> {
  const response = await fetch(`${coreBaseUrl()}/api/device`, {
    cache: 'no-store',
  })
  if (!response.ok) {
    throw new Error(`device endpoint returned ${response.status}`)
  }
  return (await response.json()) as DeviceInfo
}
