import { Play, RefreshCw, RotateCcw, Server, Square, Trash2 } from 'lucide-react'
import { useCallback, useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import {
  fetchServiceStatus,
  postServiceAction,
  type ServiceStatus,
} from '../lib/api'

/// Settings view for the optional Windows session service. Starting/stopping
/// uses the service DACL; install/uninstall still prompt for UAC.
/// `showUnavailable` keeps the section visible on platforms without the
/// service (Linux/macOS) so the entry does not silently disappear.
export function ServicePanel({
  token,
  showUnavailable = false,
}: {
  token: string
  showUnavailable?: boolean
}) {
  const { t } = useTranslation()
  const [status, setStatus] = useState<ServiceStatus | null>(null)
  const [busy, setBusy] = useState(false)
  const [note, setNote] = useState<string | null>(null)

  const load = useCallback(async () => {
    try {
      setStatus(await fetchServiceStatus(token))
    } catch {
      // older cores have no endpoint
    }
  }, [token])

  useEffect(() => {
    const initial = window.setTimeout(() => void load(), 0)
    const poll = window.setInterval(() => void load(), 3000)
    return () => {
      window.clearTimeout(initial)
      window.clearInterval(poll)
    }
  }, [load])

  const act = async (
    action: 'install' | 'uninstall' | 'start' | 'stop' | 'restart',
  ) => {
    setBusy(true)
    setNote(null)
    try {
      const result = await postServiceAction(token, action)
      setNote(result.ok ? null : result.error ?? t('service.actionFailed'))
      await load()
    } catch {
      setNote(t('service.actionFailed'))
    } finally {
      setBusy(false)
    }
  }

  if (!status) return null
  if (!status.available) {
    if (!showUnavailable) return null
    return (
      <div className="space-y-2">
        <div className="flex items-center gap-2 text-[12px] font-medium text-ink">
          <Server size={14} className="text-accent" />
          {t('service.panelTitle')}
        </div>
        <p className="text-[11px] leading-relaxed text-muted">
          {t('service.platformUnsupported')}
        </p>
      </div>
    )
  }
  const autostart = status.autostart === true
  const title = autostart
    ? t('service.autostartTitle')
    : t('service.panelTitle')
  const state = !status.installed
    ? t('service.notInstalled')
    : status.running
      ? t('service.running')
      : t('service.stopped')

  return (
    <div className="space-y-2">
      <div className="flex items-center gap-2 text-[12px] font-medium text-ink">
        <Server size={14} className="text-accent" />
        {title}
        <button
          type="button"
          className="btn btn-quiet ml-auto h-7 w-7 !px-0"
          onClick={() => void load()}
          title={t('service.refresh')}
          aria-label={t('service.refresh')}
        >
          <RefreshCw size={12} />
        </button>
      </div>
      <p className="text-[11px] text-muted">
        {t('service.status')}: <span className="font-mono">{state}</span>
      </p>
      <div className="flex flex-wrap gap-2">
        {!status.installed ? (
          <button
            type="button"
            className="btn h-8 px-2 text-[11px]"
            disabled={busy}
            onClick={() => void act('install')}
          >
            {autostart ? t('service.enableAutostart') : t('service.install')}
          </button>
        ) : (
          <>
            <button
              type="button"
              aria-pressed={status.running}
              className={
                'btn h-8 gap-1 px-2 text-[11px]' +
                (status.running ? '' : ' btn-primary')
              }
              disabled={busy}
              onClick={() => void act(status.running ? 'stop' : 'start')}
            >
              {status.running ? <Square size={12} /> : <Play size={12} />}
              {status.running ? t('service.stop') : t('service.start')}
            </button>
            <button
              type="button"
              className="btn h-8 gap-1 px-2 text-[11px]"
              disabled={busy}
              onClick={() => void act('restart')}
            >
              <RotateCcw size={12} />
              {t('service.restart')}
            </button>
            <button
              type="button"
              className="btn h-8 gap-1 px-2 text-[11px]"
              disabled={busy}
              onClick={() => void act('uninstall')}
            >
              <Trash2 size={12} />
              {autostart ? t('service.disableAutostart') : t('service.uninstall')}
            </button>
          </>
        )}
      </div>
      {note ? <p className="text-[11px] text-warn">{note}</p> : null}
      <p className="text-[11px] leading-relaxed text-muted">
        {autostart ? t('service.autostartHint') : t('service.panelHint')}
      </p>
    </div>
  )
}
