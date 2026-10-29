import { Maximize2, Minimize2, Minus, X } from 'lucide-react'
import {
  useRef,
  useState,
  type PointerEvent as ReactPointerEvent,
  type ReactNode,
} from 'react'
import { useTranslation } from 'react-i18next'
import { Logo } from './Logo'
import { shellInfo, windowAction } from '../lib/shell'
import { useAppStore } from '../store/app'

function WindowButton({
  label,
  danger,
  active,
  onClick,
  children,
}: {
  label: string
  danger?: boolean
  active?: boolean
  onClick: () => void
  children: ReactNode
}) {
  return (
    <button
      type="button"
      aria-label={label}
      title={label}
      onClick={onClick}
      className={
        'grid h-7 w-8 place-items-center rounded-md transition ' +
        (danger
          ? 'hover:bg-danger/15 hover:text-danger'
          : active
            ? 'bg-accent-soft text-accent'
            : 'text-muted hover:bg-panel hover:text-ink')
      }
    >
      {children}
    </button>
  )
}

/// Custom window chrome, rendered only inside moLink's native shells.
///
/// The square button is a fullscreen control (the whole app fills the display,
/// chrome included), not a window maximize: the session has its own immersive
/// fullscreen that shows the remote picture alone. Double-clicking the drag
/// area still maximizes the window.
export function TitleBar() {
  const { t } = useTranslation()
  const windowFullscreen = useAppStore((state) => state.windowFullscreen)
  const setWindowFullscreen = useAppStore((state) => state.setWindowFullscreen)
  const tabs = useAppStore((state) => state.tabs)
  const activeTabId = useAppStore((state) => state.activeTabId)
  const closeTab = useAppStore((state) => state.closeTab)
  const closeBehavior = useAppStore((state) => state.windowCloseBehavior)
  const setCloseBehavior = useAppStore((state) => state.setWindowCloseBehavior)
  const [prompt, setPrompt] = useState(false)
  const [remember, setRemember] = useState(false)
  // null = centred by CSS; once dragged, explicit viewport coordinates.
  const [pos, setPos] = useState<{ x: number; y: number } | null>(null)
  const dragRef = useRef<{ dx: number; dy: number } | null>(null)

  if (!shellInfo.enabled || shellInfo.platform === 'macos') return null

  // Closing the window with a remote picture open is ambiguous: the user may
  // mean "drop this session" or "keep it running in the tray". Ask once, and
  // remember the answer.
  const closeSessionTab = () => {
    const id =
      activeTabId !== 'home'
        ? activeTabId
        : tabs.length > 0
          ? tabs[tabs.length - 1]!.id
          : ''
    if (id !== '') closeTab(id)
  }
  const requestClose = () => {
    if (tabs.length === 0 || closeBehavior === 'tray') {
      windowAction('close')
      return
    }
    if (closeBehavior === 'tab') {
      closeSessionTab()
      return
    }
    setPos(null)
    setPrompt(true)
  }
  const choose = (choice: 'tray' | 'tab') => {
    if (remember) setCloseBehavior(choice)
    setPrompt(false)
    if (choice === 'tray') windowAction('close')
    else closeSessionTab()
  }

  const startDrag = (event: ReactPointerEvent<HTMLDivElement>) => {
    const card = event.currentTarget.parentElement
    if (card === null) return
    const rect = card.getBoundingClientRect()
    dragRef.current = {
      dx: event.clientX - rect.left,
      dy: event.clientY - rect.top,
    }
    setPos({ x: rect.left, y: rect.top })
    event.currentTarget.setPointerCapture(event.pointerId)
  }
  const onDrag = (event: ReactPointerEvent<HTMLDivElement>) => {
    const d = dragRef.current
    if (d === null) return
    setPos({
      x: Math.max(0, Math.min(window.innerWidth - 80, event.clientX - d.dx)),
      y: Math.max(0, Math.min(window.innerHeight - 40, event.clientY - d.dy)),
    })
  }
  const endDrag = (event: ReactPointerEvent<HTMLDivElement>) => {
    dragRef.current = null
    try {
      event.currentTarget.releasePointerCapture(event.pointerId)
    } catch {
      // The capture was already released (pointer cancelled).
    }
  }

  return (
    <>
      {prompt ? (
        <div className="fixed inset-0 z-50 bg-black/40">
          <div
            className="absolute w-[330px] rounded-xl border border-line-soft bg-surface p-4 shadow-xl"
            style={
              pos === null
                ? { left: '50%', top: '50%', transform: 'translate(-50%, -50%)' }
                : { left: pos.x, top: pos.y }
            }
          >
            <div
              className="cursor-move select-none text-[13px] font-medium text-ink"
              onPointerDown={startDrag}
              onPointerMove={onDrag}
              onPointerUp={endDrag}
              onPointerCancel={endDrag}
            >
              {t('window.closePromptTitle')}
            </div>
            <p className="mt-1 text-[11px] leading-relaxed text-muted">
              {t('window.closePromptBody', { count: tabs.length })}
            </p>
            <label className="mt-3 flex items-center gap-2 text-[11px] text-muted">
              <input
                type="checkbox"
                checked={remember}
                onChange={(event) => setRemember(event.target.checked)}
              />
              {t('window.rememberChoice')}
            </label>
            <div className="mt-4 flex justify-end gap-2">
              <button
                type="button"
                className="rounded-md px-3 py-1.5 text-[12px] text-muted hover:bg-panel"
                onClick={() => {
                  setPrompt(false)
                  setPos(null)
                }}
              >
                {t('window.cancel')}
              </button>
              <button
                type="button"
                className="rounded-md px-3 py-1.5 text-[12px] text-muted hover:bg-panel"
                onClick={() => choose('tab')}
              >
                {t('window.closeTab')}
              </button>
              <button
                type="button"
                className="rounded-md bg-accent-soft px-3 py-1.5 text-[12px] text-accent"
                onClick={() => choose('tray')}
              >
                {t('window.minimizeToTray')}
              </button>
            </div>
          </div>
        </div>
      ) : null}
    <div className="relative z-20 flex h-10 shrink-0 items-center border-b border-line-soft bg-surface/85 pl-3 pr-2 backdrop-blur">
      <div className="flex items-center">
        <Logo size={17} />
      </div>

      <div
        className="h-full flex-1"
        onPointerDown={(event) => {
          if (event.button === 0) windowAction('drag')
        }}
        onDoubleClick={() => windowAction('maximize')}
      />

      <div className="flex items-center gap-1">
        <WindowButton
          label={t('window.minimize')}
          onClick={() => windowAction('minimize')}
        >
          <Minus size={14} />
        </WindowButton>
        <WindowButton
          label={
            windowFullscreen
              ? t('window.exitFullscreen')
              : t('window.fullscreen')
          }
          active={windowFullscreen}
          onClick={() => setWindowFullscreen(!windowFullscreen)}
        >
          {windowFullscreen ? <Minimize2 size={13} /> : <Maximize2 size={13} />}
        </WindowButton>
        <WindowButton
          label={t('window.close')}
          danger
          onClick={requestClose}
        >
          <X size={15} />
        </WindowButton>
      </div>
    </div>
    </>
  )
}
