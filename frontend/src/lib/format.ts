export function formatRelative(value: number): string {
  if (!value) return '—'
  const seconds = Math.round((value - Date.now()) / 1000)
  const formatter = new Intl.RelativeTimeFormat(undefined, { numeric: 'auto' })
  if (Math.abs(seconds) < 60) return formatter.format(seconds, 'second')
  const minutes = Math.round(seconds / 60)
  if (Math.abs(minutes) < 60) return formatter.format(minutes, 'minute')
  const hours = Math.round(minutes / 60)
  if (Math.abs(hours) < 24) return formatter.format(hours, 'hour')
  return formatter.format(Math.round(hours / 24), 'day')
}

/// Directory device IDs are shown as 3-3-3 groups (AB3-DEF-GHJ). Anything
/// else (a host, an empty value) is returned unchanged.
export function formatDisplayId(value?: string | null): string {
  const raw = (value ?? '').replace(/[^A-Za-z0-9]/g, '').toUpperCase()
  if (raw.length !== 9) return value ?? ''
  return `${raw.slice(0, 3)}-${raw.slice(3, 6)}-${raw.slice(6)}`
}
