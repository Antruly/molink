import { Play, RefreshCw, RotateCcw, Server, Square } from 'lucide-react'
import { useCallback, useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import {
  fetchServiceStatus,
  postServiceAction,
  type ServiceStatus,
} from '../lib/api'
import { useAppStore } from '../store/app'

/// Home-page service card. The server persists the user's desired on/off
/// state, so a manual stop survives the next launch and an overwrite install
/// does not silently turn it back on.
export function ServiceCard() {
  const { t } = useTranslation()
  const localMode = useAppStore((state) => state.localMode)
  const token = useAppStore((state) => state.token)
  const [status, setStatus] = useState<ServiceStatus | null>(null)
  const [busy, setBusy] = useState(false)
  const [note, setNote] = useState<string | null>(null)

  const load = useCallback(async () => {
    if (!token) return
    try {
      setStatus(await fetchServiceStatus(token))
    } catch {
      // Older cores have no endpoint.
    }
  }, [token])

  useEffect(() => {
    if (!localMode) return
    const initial = window.setTimeout(() => void load(), 0)
    const poll = window.setInterval(() => void load(), 3000)
    return () => {
      window.clearTimeout(initial)
      window.clearInterval(poll)
    }
  }, [localMode, load])

  const act = async (
    action: 'install' | 'start' | 'stop' | 'restart',
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

  if (!localMode || !status?.available) return null

  const state = !status.installed
    ? t('service.notInstalled')
    : status.running
      ? t('service.running')
      : t('service.stopped')

  return (
    <div className="flex flex-wrap items-center gap-3 rounded-xl border border-line bg-panel/50 px-4 py-3">
      <Server size={18} className="shrink-0 text-accent" />
      <div className="min-w-0 flex-1">
        <div className="flex flex-wrap items-center gap-2">
          <span className="text-[13px] font-medium text-ink">
            {t('service.homeTitle')}
          </span>
          <span className="chip">{state}</span>
          <span className="text-[10px] text-muted/80">
            {status.desired ? t('service.desiredOn') : t('service.desiredOff')}
          </span>
        </div>
        <div className="mt-0.5 text-[11px] leading-relaxed text-muted">
          {t('service.homeHint')}
        </div>
        {note ? <div className="mt-1 text-[11px] text-warn">{note}</div> : null}
      </div>
      <div className="flex flex-wrap items-center gap-1.5">
        <button
          type="button"
          className="btn btn-quiet h-8 w-8 !px-0"
          onClick={() => void load()}
          title={t('service.refresh')}
          aria-label={t('service.refresh')}
        >
          <RefreshCw size={12} />
        </button>
        {!status.installed ? (
          <button
            type="button"
            className="btn btn-primary h-8 px-2 text-[11px]"
            disabled={busy}
            onClick={() => void act('install')}
          >
            {t('service.install')}
          </button>
        ) : (
          <>
            {status.running ? (
              <button
                type="button"
                className="btn h-8 gap-1 px-2 text-[11px]"
                disabled={busy}
                onClick={() => void act('stop')}
              >
                <Square size={12} />
                {t('service.stop')}
              </button>
            ) : (
              <button
                type="button"
                className="btn btn-primary h-8 gap-1 px-2 text-[11px]"
                disabled={busy}
                onClick={() => void act('start')}
              >
                <Play size={12} />
                {t('service.start')}
              </button>
            )}
            <button
              type="button"
              className="btn h-8 gap-1 px-2 text-[11px]"
              disabled={busy}
              onClick={() => void act('restart')}
            >
              <RotateCcw size={12} />
              {t('service.restart')}
            </button>
          </>
        )}
      </div>
    </div>
  )
}
