export type VideoMode = 'auto' | 'tiles' | 'stream'

/// The order the preference panel offers. Auto is first and is the default:
/// a new install follows the content instead of pinning one path.
export const VIDEO_MODE_OPTIONS: readonly VideoMode[] = ['auto', 'tiles', 'stream']

export const DEFAULT_VIDEO_MODE: VideoMode = 'auto'

/// The host may still store the legacy "mixed" spelling. Everything that is
/// not an explicit "tiles" or "stream" means the adaptive path.
export function normalizeVideoMode(value: unknown): VideoMode {
  if (value === 'tiles') return 'tiles'
  if (value === 'stream') return 'stream'
  return 'auto'
}
