import { Activity, Server } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { useAppStore } from '../../store/app'

export function StatusBar() {
  const { t } = useTranslation()
  const server = useAppStore((state) => state.server)
  const device = useAppStore((state) => state.device)
  const tabs = useAppStore((state) => state.tabs)
  const info = server?.server
  const version = server?.version ?? device?.version ?? ''
  const enabled = info?.enabled ?? true
  const online = info?.online ?? false
  const registered = info?.registered ?? false
  const active = tabs.filter(
    (tab) => tab.phase === 'live' || tab.phase === 'connecting',
  ).length
  const dot = !enabled
    ? 'bg-muted'
    : online
      ? 'bg-accent'
      : registered
        ? 'bg-warn'
        : 'bg-danger'
  const serverText = !enabled
    ? t('simple.lanOnly')
    : online
      ? t('simple.statusServerOnline')
      : registered
        ? t('simple.statusServerConnecting')
        : t('simple.statusServerOffline')
  return (
    <footer className="mt-4 flex flex-wrap items-center gap-x-4 gap-y-1 border-t border-line-soft px-1 py-2 text-[10px] text-muted">
      <span className="flex items-center gap-1.5">
        <Server size={11} />
        <span className={`h-1.5 w-1.5 rounded-full ${dot}`} />
        {t('simple.statusServer')}：{serverText}
      </span>
      <span className="flex items-center gap-1.5">
        <Activity size={11} />
        {t('simple.statusSessions')}：
        {active > 0
          ? t('simple.statusSessionsActive', { count: active })
          : t('simple.statusSessionsNone')}
      </span>
      <span className="ml-auto font-mono">
        {version || '—'}
      </span>
    </footer>
  )
}
