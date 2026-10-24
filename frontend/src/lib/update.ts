import { installUpdate, openExternal, shellInfo } from './shell'

export interface UpdateInfo {
  version: string
  url: string
  notes?: string
}

const CACHE_KEY = 'molink.update.cache'
const DISMISS_KEY = 'molink.update.dismissed'
const MAX_AGE_MS = 6 * 60 * 60 * 1000
const RELEASE_API =
  'https://api.github.com/repos/Antruly/molink/releases/latest'

function parts(version: string): number[] {
  return version
    .replace(/^v/i, '')
    .split(/[.-]/)
    .map((piece) => Number.parseInt(piece, 10))
    .map((value) => (Number.isFinite(value) ? value : 0))
}

export function isNewer(latest: string, current: string): boolean {
  const a = parts(latest)
  const b = parts(current)
  for (let i = 0; i < Math.max(a.length, b.length); i += 1) {
    const left = a[i] ?? 0
    const right = b[i] ?? 0
    if (left !== right) return left > right
  }
  return false
}

export function updateDismissed(version: string): boolean {
  try {
    return window.localStorage.getItem(DISMISS_KEY) === version
  } catch {
    return false
  }
}

export function dismissUpdate(version: string): void {
  try {
    window.localStorage.setItem(DISMISS_KEY, version)
  } catch {
    // Blocked storage is fine; the notice just reappears next launch.
  }
}

function chooseAsset(assets: { name: string; browser_download_url: string }[]):
  | string
  | null {
  const matches = (pattern: RegExp) =>
    assets.find((asset) => pattern.test(asset.name))?.browser_download_url ??
    null
  if (shellInfo.platform === 'windows') {
    return matches(/setup.*\.exe$/i) ?? matches(/\.exe$/i)
  }
  if (shellInfo.platform === 'macos') {
    return matches(/arm64\.dmg$/i) ?? matches(/\.dmg$/i)
  }
  if (shellInfo.platform === 'linux') {
    return (
      matches(/amd64\.deb$/i) ??
      matches(/x86_64\.AppImage$/i) ??
      matches(/\.AppImage$/i)
    )
  }
  return null
}

export function cachedUpdate(current: string): UpdateInfo | null {
  try {
    const raw = window.localStorage.getItem(CACHE_KEY)
    if (!raw) return null
    const parsed = JSON.parse(raw) as UpdateInfo & { at?: number }
    if (!parsed?.version || !parsed?.url) return null
    if (typeof parsed.at === 'number' && Date.now() - parsed.at > MAX_AGE_MS) {
      return null
    }
    return isNewer(parsed.version, current) ? parsed : null
  } catch {
    return null
  }
}

/// Checks the public release feed. Returns null when the network is blocked or
/// this build is already current; update checks are best-effort and must never
/// block the UI.
export async function checkForUpdate(
  current: string,
): Promise<UpdateInfo | null> {
  const cached = cachedUpdate(current)
  if (cached) return cached
  const controller = new AbortController()
  const timer = window.setTimeout(() => controller.abort(), 8000)
  try {
    const response = await fetch(RELEASE_API, {
      headers: { Accept: 'application/vnd.github+json' },
      signal: controller.signal,
    })
    if (!response.ok) return null
    const release = (await response.json()) as {
      tag_name?: string
      html_url?: string
      body?: string
      assets?: { name: string; browser_download_url: string }[]
    }
    const version = (release.tag_name ?? '').replace(/^v/i, '')
    if (!version || !isNewer(version, current)) return null
    const asset = chooseAsset(release.assets ?? [])
    const info: UpdateInfo = {
      version,
      url: asset ?? release.html_url ?? '',
      notes: release.body?.slice(0, 400),
    }
    if (!info.url) return null
    try {
      window.localStorage.setItem(
        CACHE_KEY,
        JSON.stringify({ ...info, at: Date.now() }),
      )
    } catch {
      // Not caching is harmless.
    }
    return info
  } catch {
    return null
  } finally {
    window.clearTimeout(timer)
  }
}

/// Starts an update. In the Windows shell this downloads the setup executable
/// and launches it; everywhere else it opens the release asset in the system
/// browser so the user can install it manually.
export function startUpdate(info: UpdateInfo): 'auto' | 'manual' {
  if (shellInfo.enabled && shellInfo.platform === 'windows') {
    installUpdate(info.url)
    return 'auto'
  }
  openExternal(info.url)
  return 'manual'
}
