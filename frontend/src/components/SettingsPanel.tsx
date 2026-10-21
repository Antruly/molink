import { X } from 'lucide-react'
import { useEffect } from 'react'
import { useTranslation } from 'react-i18next'
import { useAppStore } from '../store/app'
import { PreferencesControls } from './PreferencesControls'

export function SettingsPanel() {
  const { t } = useTranslation()
  const open = useAppStore((state) => state.settingsOpen)
  const setOpen = useAppStore((state) => state.setSettingsOpen)
  const device = useAppStore((state) => state.device)

  useEffect(() => {
    if (!open) return
    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape') setOpen(false)
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [open, setOpen])

  if (!open) return null

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-black/35 p-4 backdrop-blur-sm"
      onMouseDown={(event) => {
        if (event.target === event.currentTarget) setOpen(false)
      }}
    >
      <div className="w-full max-w-md rounded-2xl border border-line bg-surface p-5 shadow-2xl">
        <div className="mb-5 flex items-center justify-between">
          <h2 className="text-base font-semibold">{t('settings.title')}</h2>
          <button
            type="button"
            onClick={() => setOpen(false)}
            aria-label={t('settings.close')}
            className="grid h-8 w-8 place-items-center rounded-lg text-muted transition hover:bg-panel hover:text-ink"
          >
            <X size={16} />
          </button>
        </div>

        <PreferencesControls />

        <div className="mt-6 border-t border-line-soft pt-4 text-[11px] leading-relaxed text-muted">
          <div className="flex justify-between">
            <span>{t('settings.version')}</span>
            <span className="font-mono">{device?.version ?? '0.2.0'}</span>
          </div>
          <div className="mt-1 flex justify-between">
            <span>{t('settings.protocol')}</span>
            <span className="font-mono">v1</span>
          </div>
          <div className="mt-1 flex justify-between">
            <span>{t('settings.license')}</span>
            <span>MIT</span>
          </div>
        </div>
      </div>
    </div>
  )
}
