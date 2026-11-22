import {
  AlertTriangle,
  Play,
  RefreshCw,
  RotateCcw,
  Server,
  Square,
  Trash2,
} from 'lucide-react'
import { useCallback, useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import {
  fetchServiceStatus,
  postServiceAction,
  type ServiceStatus,
} from '../lib/api'
import {
  serviceActionFailure,
  servicePanelHints,
  serviceRecoveryHint,
} from '../lib/service-panel'

type ServiceAction = 'install' | 'uninstall' | 'start' | 'stop' | 'restart'

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
  const [noteAction, setNoteAction] = useState<ServiceAction | null>(null)

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

  const act = async (action: ServiceAction) => {
    setBusy(true)
    setNote(null)
    setNoteAction(action)
    try {
      const result = await postServiceAction(token, action)
      // Non-2xx responses arrive as ApiError whose message is the backend's
      // `error` field; keep that text instead of replacing it with a generic
      // sentence.
      if (!result.ok) setNote(result.error ?? t('service.actionFailed'))
    } catch (caught) {
      setNote(serviceActionFailure(caught, t('service.actionFailed')).message)
    } finally {
      setBusy(false)
      void load()
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
  const hints = servicePanelHints(status)
  const stale = hints.showStaleWarning
  const recovery =
    note && noteAction === 'install' ? serviceRecoveryHint(note) : null

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
      {status.path ? (
        <p
          className={
            'text-[11px] leading-relaxed ' +
            (stale ? 'text-warn' : 'text-muted')
          }
        >
          {t('service.path')}:{' '}
          <span className="break-all font-mono">{status.path}</span>
        </p>
      ) : null}
      {stale ? (
        <p className="flex items-start gap-1.5 text-[11px] text-warn">
          <AlertTriangle size={12} className="mt-0.5 shrink-0" />
          <span>
            {t('service.staleWarning')}
            {status.expected ? (
              <>
                {' '}
                <span className="break-all font-mono text-[10px] text-muted">
                  {t('service.expectedPath', { path: status.expected })}
                </span>
              </>
            ) : null}
          </span>
        </p>
      ) : null}
      <div className="flex flex-wrap items-center gap-2">
        {hints.showReregister ? (
          <button
            type="button"
            className={
              'btn h-8 gap-1 px-2 text-[11px]' +
              (status.installed ? '' : ' btn-primary')
            }
            disabled={busy}
            onClick={() => void act('install')}
          >
            <RefreshCw size={12} />
            {hints.reregisterLabel === 'install'
              ? t('service.install')
              : t('service.reregister')}
          </button>
        ) : !status.installed ? (
          <button
            type="button"
            className="btn h-8 px-2 text-[11px]"
            disabled={busy}
            onClick={() => void act('install')}
          >
            {autostart ? t('service.enableAutostart') : t('service.install')}
          </button>
        ) : null}
        {hints.showUacHint ? (
          <span className="text-[10px] text-muted">{t('service.uacHint')}</span>
        ) : null}
        {status.installed ? (
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
        ) : null}
      </div>
      {note ? (
        <p className="break-words text-[11px] text-warn">{note}</p>
      ) : null}
      {recovery === 'portable' ? (
        <p className="text-[11px] leading-relaxed text-warn">
          {t('service.portableMissing')}
        </p>
      ) : null}
      {recovery === 'manual' ? (
        <p className="text-[11px] leading-relaxed text-muted">
          {t('service.manualInstallHint')}
        </p>
      ) : null}
      <p className="text-[11px] leading-relaxed text-muted">
        {autostart
          ? t('service.autostartHint')
          : status.elevated
            ? t('service.panelHintElevated')
            : t('service.panelHint')}
      </p>
    </div>
  )
}
