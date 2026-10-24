export interface AvatarPreset {
  key: string
  color: string
  ring: string
}

export const AVATAR_PRESETS: AvatarPreset[] = [
  { key: 'preset:teal', color: '#2dd4bf', ring: 'rgba(45,212,191,.35)' },
  { key: 'preset:blue', color: '#60a5fa', ring: 'rgba(96,165,250,.35)' },
  { key: 'preset:purple', color: '#a78bfa', ring: 'rgba(167,139,250,.35)' },
  { key: 'preset:amber', color: '#fbbf24', ring: 'rgba(251,191,36,.35)' },
  { key: 'preset:rose', color: '#fb7185', ring: 'rgba(251,113,133,.35)' },
  { key: 'preset:slate', color: '#94a3b8', ring: 'rgba(148,163,184,.35)' },
]

export function presetColor(value?: string): { color: string; ring: string } | null {
  if (!value) return null
  const preset = AVATAR_PRESETS.find((item) => item.key === value)
  return preset ? { color: preset.color, ring: preset.ring } : null
}

export function initialOf(name: string): string {
  const trimmed = name.trim()
  if (!trimmed) return '?'
  return Array.from(trimmed)[0]?.toUpperCase() ?? '?'
}

export function platformEmoji(platform?: string): string {
  switch (platform) {
    case 'windows':
      return '🪟'
    case 'linux':
      return '🐧'
    case 'macos':
      return '🍎'
    case 'android':
      return '🤖'
    case 'ios':
      return '📱'
    case 'web':
      return '🌐'
    default:
      return '💻'
  }
}

/// Resize an uploaded image to a 96×96 WebP data URL, keeping it small enough
/// to sync through the directory (the server caps avatars at 32 KB).
export async function fileToAvatar(file: File): Promise<string> {
  if (!file.type.startsWith('image/')) throw new Error('not_an_image')
  if (file.size > 8 * 1024 * 1024) throw new Error('image_too_large')
  const url = URL.createObjectURL(file)
  try {
    const image = await new Promise<HTMLImageElement>((resolve, reject) => {
      const element = new Image()
      element.onload = () => resolve(element)
      element.onerror = () => reject(new Error('image_decode_failed'))
      element.src = url
    })
    const canvas = document.createElement('canvas')
    canvas.width = 96
    canvas.height = 96
    const context = canvas.getContext('2d')
    if (!context) throw new Error('canvas_unavailable')
    const side = Math.min(image.width, image.height)
    const sx = (image.width - side) / 2
    const sy = (image.height - side) / 2
    context.drawImage(image, sx, sy, side, side, 0, 0, 96, 96)
    for (const quality of [0.82, 0.65, 0.5]) {
      const data = canvas.toDataURL('image/webp', quality)
      if (data.startsWith('data:image/webp') && data.length <= 32000) {
        return data
      }
      const jpeg = canvas.toDataURL('image/jpeg', quality)
      if (jpeg.length <= 32000) return jpeg
    }
    throw new Error('image_too_large')
  } finally {
    URL.revokeObjectURL(url)
  }
}
