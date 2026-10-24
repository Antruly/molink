import { Download, X } from 'lucide-react'
import { useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { openExternal } from '../lib/shell'
import {
  checkForUpdate,
  dismissUpdate,
  startUpdate,
  updateDismissed,
  type UpdateInfo,
} from '../lib/update'
import { useAppStore } from '../store/app'

/// Checks the public GitHub release feed once per launch and offers to update.
/// In the Windows shell the primary action downloads and runs the setup
/// executable; every other platform opens the release asset for a manual
/// install. The manual link remains available either way.
export function UpdateNotice() {
  const { t } = useTranslation()
  const localMode = useAppStore((state) => state.localMode)
  const version = useAppStore(
    (state) => state.server?.version ?? state.device?.version,
  )
  const [update, setUpdate] = useState<UpdateInfo | null>(null)
  const [started, setStarted] = useState<'auto' | 'manual' | null>(null)

  useEffect(() => {
    if (!localMode || !version) return
    let alive = true
    void checkForUpdate(version).then((info) => {
      if (!alive || !info || updateDismissed(info.version)) return
      setUpdate(info)
    })
    return () => {
      alive = false
    }
  }, [localMode, version])

  if (!update) return null

  return (
    <div className="flex items-center gap-1 rounded-lg border border-accent/40 bg-accent-soft px-2 py-1 text-[11px] text-accent">
      <Download size={12} />
      <span className="hidden font-medium sm:inline">
        {t('update.available')} v{update.version}
      </span>
      <span className="font-medium sm:hidden">v{update.version}</span>
      <button
        type="button"
        onClick={() => setStarted(startUpdate(update))}
        className="rounded-md px-1.5 py-0.5 font-medium transition hover:bg-accent/15"
        title={t('update.installHint')}
      >
        {started === null
          ? t('update.install')
          : started === 'auto'
            ? t('update.installing')
            : t('update.opened')}
      </button>
      <button
        type="button"
        onClick={() => openExternal(update.url)}
        className="rounded-md px-1.5 py-0.5 text-accent/80 transition hover:bg-accent/15 hover:text-accent"
        title={t('update.manualHint')}
      >
        {t('update.manual')}
      </button>
      <button
        type="button"
        onClick={() => {
          dismissUpdate(update.version)
          setUpdate(null)
        }}
        className="grid h-5 w-5 place-items-center rounded-md text-accent/60 transition hover:bg-accent/15 hover:text-accent"
        aria-label={t('update.dismiss')}
        title={t('update.dismiss')}
      >
        <X size={11} />
      </button>
    </div>
  )
}
