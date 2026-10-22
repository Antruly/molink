import { RefreshCw, ShieldAlert } from 'lucide-react'
import { useCallback, useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import {
  fetchWindowsSessions,
  postWindowsSession,
  type WindowsSession,
} from '../lib/api'

const STATE_NAMES = [
  'active',
  'connected',
  'connect-query',
  'shadow',
  'disconnected',
  'idle',
  'listen',
  'reset',
  'down',
  'init',
]

/// Experimental: lists WTS sessions through molink-service. Session switching
/// and kicking require the app (or at least the request) to run elevated.
export function WindowsSessionsPanel({ token }: { token: string }) {
  const { t } = useTranslation()
  const [sessions, setSessions] = useState<WindowsSession[]>([])
  const [available, setAvailable] = useState(false)
  const [note, setNote] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)

  const refresh = useCallback(async () => {
    if (!token) return
    setBusy(true)
    try {
      const result = await fetchWindowsSessions(token)
      setSessions(result.sessions ?? [])
      setAvailable(result.available !== false && result.ok)
      setNote(
        result.error === 'service_unavailable'
          ? t('settings.windowsServiceMissing')
          : result.error === 'windows_only'
            ? null
            : result.error ?? null,
      )
    } catch {
      setNote(t('error.settings_failed'))
    } finally {
      setBusy(false)
    }
  }, [token, t])

  useEffect(() => {
    const timer = window.setTimeout(() => void refresh(), 0)
    return () => window.clearTimeout(timer)
  }, [refresh])

  if (!available && note === null) return null

  const act = async (action: 'switch' | 'kick', id: number) => {
    setBusy(true)
    try {
      const result = await postWindowsSession(token, action, id)
      setNote(
        result.ok
          ? null
          : result.error === 'admin_required'
            ? t('settings.windowsAdminRequired')
            : (result.error ?? t('error.settings_failed')),
      )
      await refresh()
    } catch {
      setNote(t('error.settings_failed'))
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="space-y-2">
      <div className="flex items-center gap-2 text-[12px] font-medium text-ink">
        <ShieldAlert size={14} className="text-accent" />
        {t('settings.windowsSessions')}
        <button
          type="button"
          onClick={() => void refresh()}
          className="btn btn-quiet ml-auto h-7 w-7 !px-0"
          title={t('settings.refresh')}
          aria-label={t('settings.refresh')}
          disabled={busy}
        >
          <RefreshCw size={12} className={busy ? 'animate-spin' : ''} />
        </button>
      </div>
      <p className="text-[11px] leading-relaxed text-muted">
        {t('settings.windowsSessionsHint')}
      </p>
      {sessions.length === 0 ? (
        <p className="text-[11px] text-muted">{t('settings.windowsNoSessions')}</p>
      ) : (
        <ul className="divide-y divide-line-soft rounded-lg border border-line bg-panel/60">
          {sessions.map((session) => {
            const state = STATE_NAMES[session.state] ?? String(session.state)
            const console =
              session.station.toLowerCase().includes('console') ||
              session.station.toLowerCase().includes('services')
            return (
              <li
                key={session.id}
                className="flex items-center gap-2 px-3 py-2 text-[11px]"
              >
                <span className="font-mono text-muted">#{session.id}</span>
                <span className="min-w-0 flex-1 truncate">
                  {session.user || session.station || '—'}
                  <span className="ml-1 text-muted/70">{state}</span>
                </span>
                {!console ? (
                  <>
                    <button
                      type="button"
                      className="btn h-6 px-1.5 text-[10px]"
                      disabled={busy}
                      onClick={() => void act('switch', session.id)}
                    >
                      {t('settings.windowsSwitchConsole')}
                    </button>
                    <button
                      type="button"
                      className="btn h-6 px-1.5 text-[10px]"
                      disabled={busy}
                      onClick={() => void act('kick', session.id)}
                    >
                      {t('settings.windowsKick')}
                    </button>
                  </>
                ) : null}
              </li>
            )
          })}
        </ul>
      )}
      {note ? <p className="text-[11px] text-warn">{note}</p> : null}
    </div>
  )
}
