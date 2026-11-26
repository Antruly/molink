import { GripHorizontal, Settings2, X } from 'lucide-react'
import {
  useEffect,
  useRef,
  useState,
  type PointerEvent as ReactPointerEvent,
  type ReactNode,
} from 'react'
import { useTranslation } from 'react-i18next'

export interface ToolbarItem {
  key: string
  label: string
  icon?: ReactNode
  onSelect: () => void
  disabled?: boolean
  active?: boolean
}

/// A draggable, collapsible toolbar for the non-desktop pages. The desktop
/// page keeps its own picture toolbar; this one only carries panel actions
/// (upload, copy, font size...) plus the window controls.
export function FloatingToolbar({
  items,
  label,
}: {
  items: ToolbarItem[]
  /// Accessible name for the bar (usually the page title).
  label: string
}) {
  const { t } = useTranslation()
  const [open, setOpen] = useState(true)
  const [pos, setPos] = useState<{ x: number; y: number } | null>(null)
  const barRef = useRef<HTMLDivElement>(null)
  const drag = useRef<{
    offsetX: number
    offsetY: number
    startX: number
    startY: number
    moved: boolean
  } | null>(null)
  const suppressClick = useRef(false)

  useEffect(() => {
    if (!open) return
    const onKey = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return
      setOpen(false)
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [open])

  const handleDown = (event: ReactPointerEvent<HTMLButtonElement>) => {
    event.preventDefault()
    event.stopPropagation()
    const bar = barRef.current
    if (!bar) return
    const parent = bar.offsetParent as HTMLElement | null
    const parentRect = parent?.getBoundingClientRect()
    const barRect = bar.getBoundingClientRect()
    drag.current = {
      offsetX: event.clientX - barRect.left,
      offsetY: event.clientY - barRect.top,
      startX: event.clientX,
      startY: event.clientY,
      moved: false,
    }
    setPos({
      x: barRect.left - (parentRect?.left ?? 0),
      y: barRect.top - (parentRect?.top ?? 0),
    })
    event.currentTarget.setPointerCapture(event.pointerId)
  }

  const handleMove = (event: ReactPointerEvent<HTMLButtonElement>) => {
    const state = drag.current
    const bar = barRef.current
    if (!state || !bar) return
    if (
      Math.hypot(event.clientX - state.startX, event.clientY - state.startY) > 4
    ) {
      state.moved = true
    }
    const parent = bar.offsetParent as HTMLElement | null
    const parentRect = parent?.getBoundingClientRect()
    const maxX = Math.max(4, (parentRect?.width ?? window.innerWidth) - bar.offsetWidth - 4)
    const maxY = Math.max(4, (parentRect?.height ?? window.innerHeight) - bar.offsetHeight - 4)
    setPos({
      x: Math.min(
        maxX,
        Math.max(4, event.clientX - (parentRect?.left ?? 0) - state.offsetX),
      ),
      y: Math.min(
        maxY,
        Math.max(4, event.clientY - (parentRect?.top ?? 0) - state.offsetY),
      ),
    })
  }

  const handleUp = () => {
    if (drag.current?.moved) {
      suppressClick.current = true
      window.setTimeout(() => {
        suppressClick.current = false
      }, 0)
    }
    drag.current = null
  }

  if (!open) {
    return (
      <button
        type="button"
        onClick={() => setOpen(true)}
        className="pointer-events-auto absolute right-3 top-3 z-40 grid h-10 w-10 place-items-center rounded-full border border-line bg-surface/90 text-muted shadow-lg backdrop-blur transition hover:text-ink"
        aria-label={label}
        title={label}
      >
        <Settings2 size={16} />
      </button>
    )
  }

  return (
    <div
      ref={barRef}
      role="toolbar"
      aria-label={label}
      data-local-input="true"
      style={pos ? { left: pos.x, top: pos.y, transform: 'none' } : undefined}
      className={
        'pointer-events-auto absolute z-40 flex max-w-[96%] items-center gap-0.5 overflow-x-auto rounded-full border border-line bg-surface/95 px-1.5 py-1 text-ink shadow-lg backdrop-blur ' +
        (pos ? '' : 'left-1/2 top-3 -translate-x-1/2')
      }
      onPointerDown={(event) => event.stopPropagation()}
    >
      <button
        type="button"
        onPointerDown={handleDown}
        onPointerMove={handleMove}
        onPointerUp={handleUp}
        onPointerCancel={handleUp}
        onClick={() => {
          if (suppressClick.current) {
            suppressClick.current = false
          }
        }}
        className="grid h-7 w-5 shrink-0 cursor-move touch-none place-items-center rounded text-muted/70 transition hover:bg-panel hover:text-ink"
        title={t('session.moveToolbar')}
        aria-label={t('session.moveToolbar')}
      >
        <GripHorizontal size={13} />
      </button>
      {items.map((item) => (
        <button
          key={item.key}
          type="button"
          onClick={item.onSelect}
          disabled={item.disabled}
          title={item.label}
          aria-label={item.label}
          aria-pressed={item.active}
          className={
            'flex h-7 shrink-0 items-center gap-1 rounded-full px-2 text-[11px] transition disabled:opacity-40 ' +
            (item.active
              ? 'bg-accent-soft text-accent'
              : 'text-muted hover:bg-panel hover:text-ink')
          }
        >
          {item.icon}
          <span className="whitespace-nowrap">{item.label}</span>
        </button>
      ))}
      <button
        type="button"
        onClick={() => setOpen(false)}
        className="grid h-7 w-7 shrink-0 place-items-center rounded-full text-muted transition hover:bg-panel hover:text-ink"
        title={t('session.collapseToolbar')}
        aria-label={t('session.collapseToolbar')}
      >
        <X size={13} />
      </button>
    </div>
  )
}
