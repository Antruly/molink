/** Format a host for use in a URL authority (IPv6 literals need brackets). */
export function formatHost(host: string): string {
  const value = host.trim()
  if (value.startsWith('[') && value.endsWith(']')) return value
  return value.includes(':') ? `[${value}]` : value
}
