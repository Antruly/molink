import { Maximize2, Minimize2, Minus, X } from 'lucide-react'
import type { ReactNode } from 'react'
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

  if (!shellInfo.enabled || shellInfo.platform === 'macos') return null

  return (
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
          onClick={() => windowAction('close')}
        >
          <X size={15} />
        </WindowButton>
      </div>
    </div>
  )
}
