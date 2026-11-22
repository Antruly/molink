/// Addresses shown for this device, as `ip:port` strings.
///
/// The core already orders private LAN addresses first; keep that order rather
/// than re-sorting. An empty list falls back to the host the page was served
/// from, so the card never goes blank on a machine whose interfaces the core
/// could not enumerate.
export function deviceEndpoints(
  addresses: readonly string[] | null | undefined,
  port: number | string | null | undefined,
  fallbackHost: string,
): string[] {
  const listed = (addresses ?? [])
    .map((value) => value.trim())
    .filter((value) => value.length > 0)
  const hosts =
    listed.length > 0
      ? listed
      : [fallbackHost.trim()].filter((value) => value.length > 0)
  const portText =
    port === null || port === undefined ? '' : String(port).trim()
  const suffix = portText === '' ? '' : `:${portText}`
  return hosts.map((host) => `${host}${suffix}`)
}

/// The first (primary) host, without a port. Used for input placeholders.
export function primaryHost(
  addresses: readonly string[] | null | undefined,
  fallbackHost: string,
): string {
  const [first] = deviceEndpoints(addresses, null, fallbackHost)
  return first ?? ''
}
