import { Maximize2, Minimize2, Minus, X } from 'lucide-react'
import { useEffect, useRef, type ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { shellInfo } from '../lib/shell'
import { useAppStore, type SessionTab } from '../store/app'

/// Window shell shared by the standalone files/chat/terminal pages: a title
/// bar with minimize/close/fullscreen, and a content area. The desktop page
/// keeps its canvas and picture toolbar; everything here is about the tab
/// being a window of its own.
export function SessionChrome({
  tab,
  title,
  status,
  onMinimize,
  onClose,
  children,
}: {
  tab: SessionTab
  title: string
  /// Small status text/dot shown after the title.
  status?: ReactNode
  onMinimize: () => void
  onClose: () => void
  children: ReactNode
}) {
  const { t } = useTranslation()
  const rootRef = useRef<HTMLDivElement>(null)
  const immersive = useAppStore((state) => state.immersive)
  const setImmersive = useAppStore((state) => state.setImmersive)
  const setWindowFullscreenState = useAppStore(
    (state) => state.setWindowFullscreen,
  )

  const toggleFullscreen = () => {
    const next = !immersive
    if (next) {
      setImmersive(true)
      if (shellInfo.enabled) setWindowFullscreenState(true)
      else void rootRef.current?.requestFullscreen?.().catch(() => undefined)
    } else {
      setImmersive(false)
      if (shellInfo.enabled) setWindowFullscreenState(false)
      else if (document.fullscreenElement) void document.exitFullscreen()
    }
  }

  // Esc leaves fullscreen, matching the desktop page. The floating toolbar
  // collapses itself on Esc first, so this only fires once the chrome is the
  // remaining thing in immersive mode. Escape typed into the terminal, chat
  // box or a path field belongs to that control, exactly like the desktop's
  // remote keyboard: it must not also drop the window out of fullscreen.
  useEffect(() => {
    if (!immersive) return
    const onKey = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return
      const target = event.target
      if (
        target instanceof Element &&
        target.closest(
          '.xterm, input, textarea, select, [contenteditable="true"]',
        ) !== null
      ) {
        return
      }
      event.preventDefault()
      setImmersive(false)
      if (shellInfo.enabled) setWindowFullscreenState(false)
      else if (document.fullscreenElement) void document.exitFullscreen()
    }
    window.addEventListener('keydown', onKey, true)
    return () => window.removeEventListener('keydown', onKey, true)
  }, [immersive, setImmersive, setWindowFullscreenState])

  // Leaving the page must not strand the window in fullscreen.
  useEffect(() => {
    return () => {
      if (useAppStore.getState().immersive) {
        setImmersive(false)
        setWindowFullscreenState(false)
        if (!shellInfo.enabled && document.fullscreenElement) {
          void document.exitFullscreen()
        }
      }
    }
  }, [setImmersive, setWindowFullscreenState])

  return (
    <div
      ref={rootRef}
      className="relative flex min-h-0 flex-1 flex-col bg-canvas"
    >
      {immersive ? null : (
        <div className="flex h-10 shrink-0 items-center gap-2 border-b border-line-soft bg-surface/40 px-2">
          <span
            className={
              'h-1.5 w-1.5 shrink-0 rounded-full ' +
              (tab.phase === 'live'
                ? 'bg-accent'
                : tab.phase === 'connecting'
                  ? 'animate-pulse bg-warn'
                  : tab.phase === 'error'
                    ? 'bg-danger'
                    : 'bg-muted/60')
            }
            aria-hidden="true"
          />
          <span className="min-w-0 truncate text-[12px] font-medium text-ink">
            {title}
          </span>
          {status}
          <div className="ml-auto flex shrink-0 items-center gap-0.5">
            <button
              type="button"
              onClick={toggleFullscreen}
              className="grid h-7 w-7 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink"
              title={
                immersive ? t('session.exitFullscreen') : t('session.fullscreen')
              }
              aria-label={
                immersive ? t('session.exitFullscreen') : t('session.fullscreen')
              }
            >
              {immersive ? <Minimize2 size={14} /> : <Maximize2 size={14} />}
            </button>
            <button
              type="button"
              onClick={onMinimize}
              className="grid h-7 w-7 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink"
              title={t('tabs.minimize')}
              aria-label={t('tabs.minimize')}
            >
              <Minus size={15} />
            </button>
            <button
              type="button"
              onClick={onClose}
              className="grid h-7 w-7 place-items-center rounded-md text-muted transition hover:bg-danger/15 hover:text-danger"
              title={t('session.disconnect')}
              aria-label={t('session.disconnect')}
            >
              <X size={15} />
            </button>
          </div>
        </div>
      )}
      <div className="relative min-h-0 flex-1">{children}</div>
    </div>
  )
}
