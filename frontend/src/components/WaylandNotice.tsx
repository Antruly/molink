import { Info, X } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import {
  dismissWaylandNotice,
  shouldShowWaylandNotice,
  waylandNoticeDismissed,
} from '../lib/session-type'
import { useAppStore } from '../store/app'

/// A host running a Wayland session can only capture and control XWayland
/// applications. Say so once instead of letting the missing native windows
/// look like a broken feature. It is an inline bar (never a dialog) and its
/// dismissal is remembered in localStorage.
export function WaylandNotice() {
  const { t } = useTranslation()
  const localMode = useAppStore((state) => state.localMode)
  const sessionType = useAppStore((state) => state.server?.session_type)
  const [dismissed, setDismissed] = useState(waylandNoticeDismissed)

  if (!localMode || !shouldShowWaylandNotice(sessionType, dismissed)) {
    return null
  }

  const close = () => {
    dismissWaylandNotice()
    setDismissed(true)
  }

  return (
    <div
      role="status"
      className="mb-3 flex flex-wrap items-start gap-2 rounded-xl border border-warn/40 bg-warn/10 px-3 py-2 text-[11px]"
    >
      <Info size={15} className="mt-0.5 shrink-0 text-warn" />
      <div className="min-w-0 flex-1">
        <div className="text-[12px] font-medium text-ink">
          {t('wayland.title')}
        </div>
        <div className="mt-0.5 leading-relaxed text-muted">
          {t('wayland.body')}
        </div>
      </div>
      <button
        type="button"
        onClick={close}
        className="grid h-5 w-5 shrink-0 place-items-center rounded-md text-muted transition hover:bg-warn/15 hover:text-ink"
        aria-label={t('wayland.dismiss')}
        title={t('wayland.dismiss')}
      >
        <X size={12} />
      </button>
    </div>
  )
}
