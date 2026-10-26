import { Download, ShieldCheck } from 'lucide-react'
import { useCallback, useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import {
  fetchServiceStatus,
  postServiceAction,
  type ServiceStatus,
} from '../lib/api'
import { useAppStore } from '../store/app'

const DISMISS_KEY = 'molink.servicePromptDismissed'

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => window.setTimeout(resolve, ms))
}

/// Portable builds: offer to install the optional LocalSystem helper. The
/// installer build asks the same question with a checked-by-default task, so
/// this dialog only appears when the service is genuinely missing.
export function ServicePrompt() {
  const { t } = useTranslation()
  const localMode = useAppStore((state) => state.localMode)
  const token = useAppStore((state) => state.token)
  const [status, setStatus] = useState<ServiceStatus | null>(null)
  const [dismissed, setDismissed] = useState(() => {
    try {
      return window.sessionStorage.getItem(DISMISS_KEY) === '1'
    } catch {
      return false
    }
  })
  const [busy, setBusy] = useState(false)
  const [note, setNote] = useState<string | null>(null)

  const load = useCallback(async () => {
    if (!token) return
    try {
      setStatus(await fetchServiceStatus(token))
    } catch {
      // Older cores do not expose /api/service; fail silently.
    }
  }, [token])

  useEffect(() => {
    if (!localMode) return
    const timer = window.setTimeout(() => void load(), 0)
    return () => window.clearTimeout(timer)
  }, [localMode, load])

  const dismiss = () => {
    setDismissed(true)
    try {
      window.sessionStorage.setItem(DISMISS_KEY, '1')
    } catch {
      // ignore
    }
  }

  const install = async () => {
    setBusy(true)
    setNote(null)
    try {
      const result = await postServiceAction(token, 'install')
      if (!result.ok) {
        setNote(result.error ?? t('service.installFailed'))
        return
      }
      // The elevated helper runs asynchronously; wait for the service to
      // appear, then start it (the app owns its lifecycle).
      for (let i = 0; i < 30; i += 1) {
        await sleep(1000)
        const current = await fetchServiceStatus(token)
        setStatus(current)
        if (current.installed) {
          await postServiceAction(token, 'start')
          await load()
          return
        }
      }
      setNote(t('service.installFailed'))
    } catch {
      setNote(t('service.installFailed'))
    } finally {
      setBusy(false)
    }
  }

  // `autostart` is the Linux systemd login service; the lock-screen prompt
  // only applies to the Windows session helper.
  if (
    !localMode ||
    dismissed ||
    !status?.available ||
    status.autostart ||
    status.installed
  ) {
    return null
  }

  return (
    <div
      data-local-input="true"
      className="fixed inset-0 z-[60] grid place-items-center bg-black/45 p-4 backdrop-blur-sm"
    >
      <div className="w-[min(92vw,30rem)] rounded-2xl border border-line bg-surface p-5 shadow-2xl">
        <div className="flex items-start gap-3">
          <ShieldCheck size={22} className="mt-0.5 shrink-0 text-accent" />
          <div className="min-w-0">
            <h2 className="text-[15px] font-semibold text-ink">
              {t('service.promptTitle')}
            </h2>
            <p className="mt-2 text-[12px] leading-relaxed text-muted">
              {t('service.promptBody')}
            </p>
            <ul className="mt-2 list-inside list-disc text-[11px] leading-relaxed text-muted">
              <li>{t('service.promptLockScreen')}</li>
              <li>{t('service.promptSession')}</li>
              <li>{t('service.promptElevation')}</li>
            </ul>
            {note ? (
              <p className="mt-2 text-[11px] text-warn">{note}</p>
            ) : null}
            <div className="mt-4 flex items-center gap-2">
              <button
                type="button"
                className="btn btn-primary h-9 gap-1.5 px-3 text-[12px]"
                disabled={busy}
                onClick={() => void install()}
              >
                <Download size={14} />
                {busy ? t('service.installing') : t('service.install')}
              </button>
              <button
                type="button"
                className="btn h-9 px-3 text-[12px]"
                disabled={busy}
                onClick={dismiss}
              >
                {t('service.later')}
              </button>
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}
