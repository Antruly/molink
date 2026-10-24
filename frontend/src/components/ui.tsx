import { useEffect, type ReactNode } from 'react'
import { X } from 'lucide-react'

/// Small centred dialog used by the simple home. `data-local-input` keeps the
/// native shell from forwarding clicks inside it to the remote session.
export function Modal({
  open,
  title,
  onClose,
  footer,
  children,
}: {
  open: boolean
  title: string
  onClose: () => void
  footer?: ReactNode
  children: ReactNode
}) {
  useEffect(() => {
    if (!open) return
    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape') onClose()
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [open, onClose])

  if (!open) return null

  return (
    <div
      data-local-input="true"
      className="fixed inset-0 z-50 flex items-center justify-center bg-black/35 p-4 backdrop-blur-sm"
      onMouseDown={(event) => {
        if (event.target === event.currentTarget) onClose()
      }}
    >
      <div className="max-h-[88vh] w-full max-w-sm overflow-y-auto rounded-2xl border border-line bg-surface p-5 shadow-2xl">
        <div className="mb-4 flex items-center justify-between gap-3">
          <h2 className="text-[14px] font-semibold text-ink">{title}</h2>
          <button
            type="button"
            onClick={onClose}
            aria-label={title}
            className="grid h-7 w-7 place-items-center rounded-lg text-muted transition hover:bg-panel hover:text-ink"
          >
            <X size={15} />
          </button>
        </div>
        {children}
        {footer ? (
          <div className="mt-4 flex justify-end gap-2">{footer}</div>
        ) : null}
      </div>
    </div>
  )
}
