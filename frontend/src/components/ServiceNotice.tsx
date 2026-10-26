import { Download, RefreshCw, Server, X } from 'lucide-react'
import { useCallback, useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import {
  fetchServiceStatus,
  postServiceAction,
  type ServiceStatus,
} from '../lib/api'
import { useAppStore } from '../store/app'

const DISMISS_KEY = 'molink.serviceNoticeDismissed'

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => window.setTimeout(resolve, ms))
}

/// Main-page notification bar for the optional Windows background service.
///
/// It replaces the old first-run dialog: a missing service shows a one-click
/// install, and a service that was supposed to run but is not running (a
/// failed start, a crash, or a denied UAC prompt) offers a restart. Both
/// disappear as soon as the service is healthy.
export function ServiceNotice() {
  const { t } = useTranslation()
  const localMode = useAppStore((state) => state.localMode)
  const token = useAppStore((state) => state.token)
  const [status, setStatus] = useState<ServiceStatus | null>(null)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [dismissed, setDismissed] = useState(() => {
    try {
      return window.sessionStorage.getItem(DISMISS_KEY) === '1'
    } catch {
      return false
    }
  })

  const load = useCallback(async () => {
    if (!token) return
    try {
      setStatus(await fetchServiceStatus(token))
    } catch {
      // Older cores do not expose /api/service; stay quiet.
    }
  }, [token])

  useEffect(() => {
    if (!localMode) return
    const initial = window.setTimeout(() => void load(), 0)
    const timer = window.setInterval(() => void load(), 5000)
    return () => {
      window.clearTimeout(initial)
      window.clearInterval(timer)
    }
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
    setError(null)
    try {
      const result = await postServiceAction(token, 'install')
      if (!result.ok) {
        setError(result.error ?? t('service.installFailed'))
        return
      }
      // The elevated helper installs asynchronously; wait for the service to
      // appear and then start it (the app owns its lifecycle).
      for (let i = 0; i < 30; i += 1) {
        await sleep(1000)
        const current = await fetchServiceStatus(token)
        setStatus(current)
        if (current.installed) {
          const started = await postServiceAction(token, 'start')
          if (!started.ok) {
            setError(started.error ?? t('service.actionFailed'))
          }
          await load()
          return
        }
      }
      setError(t('service.installFailed'))
    } catch {
      setError(t('service.installFailed'))
    } finally {
      setBusy(false)
    }
  }

  const restart = async () => {
    setBusy(true)
    setError(null)
    try {
      const result = await postServiceAction(token, 'restart')
      if (!result.ok) {
        setError(result.error ?? t('service.actionFailed'))
      }
      await load()
    } catch {
      setError(t('service.actionFailed'))
    } finally {
      setBusy(false)
    }
  }

  if (!localMode || !status) return null
  // Linux/macOS have the systemd autostart variant, not this helper.
  if (!status.available || status.autostart) return null

  const needsInstall = !status.installed
  // `desired` is the user's saved preference. Installed but not running while
  // desired means the start failed (UAC, a crash, a broken install) rather
  // than an intentional stop.
  const needsRestart = status.installed && !status.running && status.desired
  if (!needsInstall && !needsRestart) return null
  // The install suggestion is dismissible; a service that should be running
  // but is not keeps the restart button visible.
  if (dismissed && needsInstall) return null

  return (
    <div
      data-local-input="true"
      className={
        'mb-3 flex flex-wrap items-center gap-3 rounded-xl border px-3 py-2 ' +
        (needsInstall
          ? 'border-accent/30 bg-accent-soft/40'
          : 'border-warn/35 bg-warn/10')
      }
    >
      <Server
        size={16}
        className={needsInstall ? 'text-accent' : 'text-warn'}
      />
      <div className="min-w-0 flex-1">
        <div className="text-[12px] font-medium text-ink">
          {needsInstall
            ? t('service.noticeInstallTitle')
            : t('service.noticeRestartTitle')}
        </div>
        <div className="mt-0.5 text-[11px] leading-relaxed text-muted">
          {needsInstall
            ? t('service.noticeInstallBody')
            : t('service.noticeRestartBody')}
        </div>
        {error ? (
          <div className="mt-1 text-[11px] text-warn">{error}</div>
        ) : null}
      </div>
      <button
        type="button"
        className="btn btn-primary h-8 shrink-0 gap-1.5 px-3 text-[11px]"
        disabled={busy}
        onClick={() => void (needsInstall ? install() : restart())}
      >
        {needsInstall ? <Download size={13} /> : <RefreshCw size={13} />}
        {busy
          ? needsInstall
            ? t('service.installing')
            : t('service.restarting')
          : needsInstall
            ? t('service.install')
            : t('service.restart')}
      </button>
      {needsInstall ? (
        <button
          type="button"
          className="btn btn-quiet h-8 w-8 !px-0"
          onClick={dismiss}
          title={t('service.later')}
          aria-label={t('service.later')}
        >
          <X size={13} />
        </button>
      ) : null}
    </div>
  )
}
