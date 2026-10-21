import { Maximize2, Minus, X } from 'lucide-react'
import type { ReactNode } from 'react'
import { Logo } from './Logo'
import { shellInfo, windowAction } from '../lib/shell'

function WindowButton({
  label,
  danger,
  onClick,
  children,
}: {
  label: string
  danger?: boolean
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
        'grid h-7 w-8 place-items-center rounded-md text-muted transition ' +
        (danger
          ? 'hover:bg-danger/15 hover:text-danger'
          : 'hover:bg-panel hover:text-ink')
      }
    >
      {children}
    </button>
  )
}

/// Custom window chrome, rendered only inside moLink's native shells.
export function TitleBar() {
  if (!shellInfo.enabled || shellInfo.platform === 'macos') return null
  const mac = false

  return (
    <div
      className={
        'relative z-20 flex h-10 shrink-0 items-center border-b border-line-soft bg-surface/90 pr-2 backdrop-blur ' +
        (mac ? 'pl-[78px]' : 'pl-3')
      }
    >
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

      {!mac ? (
        <div className="flex items-center gap-1">
          <WindowButton
            label="Minimize"
            onClick={() => windowAction('minimize')}
          >
            <Minus size={14} />
          </WindowButton>
          <WindowButton
            label="Maximize"
            onClick={() => windowAction('maximize')}
          >
            <Maximize2 size={13} />
          </WindowButton>
          <WindowButton
            label="Close"
            danger
            onClick={() => windowAction('close')}
          >
            <X size={15} />
          </WindowButton>
        </div>
      ) : null}
    </div>
  )
}
