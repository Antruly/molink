import { iconFor, type ChatIconId } from '../../lib/chat'

/// Inline, table-driven file glyphs. No font or image asset is loaded: a
/// bubble must render the same on a host with no network.
export const FILE_ICON_COLORS: Record<ChatIconId, string> = {
  doc: '#3b82f6',
  sheet: '#22c55e',
  slides: '#f97316',
  pdf: '#ef4444',
  archive: '#f59e0b',
  image: '#8b5cf6',
  video: '#ec4899',
  audio: '#14b8a6',
  text: '#64748b',
  code: '#06b6d4',
  file: '#94a3b8',
}

function glyph(id: ChatIconId) {
  switch (id) {
    case 'doc':
      return (
        <>
          <path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z" />
          <path d="M14 3v5h5" />
          <path d="M9 13h6M9 17h6M9 9h1" />
        </>
      )
    case 'sheet':
      return (
        <>
          <rect x="4" y="4" width="16" height="16" rx="2" />
          <path d="M4 10h16M10 4v16M4 15h16M15 10v10" />
        </>
      )
    case 'slides':
      return (
        <>
          <rect x="3" y="4" width="18" height="12" rx="2" />
          <path d="M12 16v4M8 20h8M10 9l4 2-4 2z" />
        </>
      )
    case 'pdf':
      return (
        <>
          <path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z" />
          <path d="M14 3v5h5" />
          <path d="M9 13h3a1.5 1.5 0 0 1 0 3H9v3" />
          <path d="M15 13v6" />
        </>
      )
    case 'archive':
      return (
        <>
          <path d="M4 8h16v11a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2z" />
          <path d="M3 8l2-4h14l2 4" />
          <path d="M10 8v3h4V8" />
        </>
      )
    case 'image':
      return (
        <>
          <rect x="3" y="4" width="18" height="16" rx="2" />
          <circle cx="8.5" cy="9.5" r="1.5" />
          <path d="M21 16l-5-5L6 20" />
        </>
      )
    case 'video':
      return (
        <>
          <rect x="3" y="5" width="18" height="14" rx="2" />
          <path d="M10 9.5l5 2.5-5 2.5z" />
        </>
      )
    case 'audio':
      return (
        <>
          <path d="M9 18V6l10-2v12" />
          <circle cx="6.5" cy="18" r="2.5" />
          <circle cx="16.5" cy="16" r="2.5" />
        </>
      )
    case 'text':
      return (
        <>
          <path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z" />
          <path d="M14 3v5h5" />
          <path d="M9 12h6M9 16h6M9 8h2" />
        </>
      )
    case 'code':
      return (
        <>
          <path d="M9 8l-4 4 4 4M15 8l4 4-4 4M13 5l-2 14" />
        </>
      )
    default:
      return (
        <>
          <path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z" />
          <path d="M14 3v5h5" />
        </>
      )
  }
}

export function AttachmentIcon({
  name,
  icon,
  size = 36,
  className = '',
}: {
  name: string
  icon?: ChatIconId
  size?: number
  className?: string
}) {
  const id = icon ?? iconFor(name)
  const color = FILE_ICON_COLORS[id]
  return (
    <span
      aria-hidden="true"
      className={'grid shrink-0 place-items-center rounded-lg ' + className}
      style={{ width: size, height: size, color, background: `${color}20` }}
    >
      <svg
        width={Math.round(size * 0.56)}
        height={Math.round(size * 0.56)}
        viewBox="0 0 24 24"
        fill="none"
        stroke="currentColor"
        strokeWidth={1.6}
        strokeLinecap="round"
        strokeLinejoin="round"
      >
        {glyph(id)}
      </svg>
    </span>
  )
}
