import { House, Plus, X } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { openSessionWindow } from '../lib/windows'
import { formatDisplayId } from '../lib/format'
import { useAppStore, type SessionTab } from '../store/app'

function StatusDot({ tab }: { tab: SessionTab }) {
  if (tab.phase === 'live') {
    return <span className="h-1.5 w-1.5 rounded-full bg-accent" />
  }
  if (tab.phase === 'connecting') {
    return <span className="h-1.5 w-1.5 animate-pulse rounded-full bg-warn" />
  }
  if (tab.phase === 'error') {
    return <span className="h-1.5 w-1.5 rounded-full bg-danger" />
  }
  return <span className="h-1.5 w-1.5 rounded-full bg-muted/60" />
}

export function TabBar() {
  const { t } = useTranslation()
  const tabs = useAppStore((state) => state.tabs)
  const activeTabId = useAppStore((state) => state.activeTabId)
  const setActiveTab = useAppStore((state) => state.setActiveTab)
  const closeTab = useAppStore((state) => state.closeTab)
  const localMode = useAppStore((state) => state.localMode)

  return (
    <div className="flex h-11 shrink-0 items-center gap-1 overflow-x-auto border-b border-line-soft bg-surface/40 px-2">
      <button
        type="button"
        onClick={() => setActiveTab('home')}
        className={
          'flex shrink-0 items-center gap-1.5 rounded-lg px-3 py-1.5 text-[12px] transition ' +
          (activeTabId === 'home'
            ? 'bg-accent-soft text-accent'
            : 'text-muted hover:bg-panel hover:text-ink')
        }
      >
        <House size={14} />
        {t('tabs.home')}
      </button>

      {tabs.map((tab) => (
        <div
          key={tab.id}
          draggable
          onDragStart={(event) => {
            event.dataTransfer.setData('text/plain', tab.id)
            event.dataTransfer.effectAllowed = 'move'
          }}
          onDragEnd={(event) => {
            const outside =
              event.clientX <= 0 ||
              event.clientY <= 0 ||
              event.clientX >= window.innerWidth ||
              event.clientY >= window.innerHeight
            if (!outside || !localMode) return
            if (
              openSessionWindow({
                host: tab.host,
                port: tab.port,
                token: tab.token,
                label: tab.label,
              })
            ) {
              closeTab(tab.id)
            }
          }}
          className={
            'group flex shrink-0 items-center gap-1.5 rounded-lg pl-3 pr-1.5 text-[12px] transition ' +
            (activeTabId === tab.id
              ? 'bg-accent-soft text-accent'
              : 'text-muted hover:bg-panel hover:text-ink')
          }
        >
          <button
            type="button"
            onClick={() => setActiveTab(tab.id)}
            className="flex max-w-[190px] items-center gap-2 py-1.5"
            title={tab.deviceId ? `${formatDisplayId(tab.deviceId)} · ${tab.label}` : `${tab.host}:${tab.port}`}
          >
            <StatusDot tab={tab} />
            <span className="truncate">{tab.label}</span>
          </button>
          <button
            type="button"
            onClick={() => closeTab(tab.id)}
            aria-label={t('tabs.close')}
            title={t('tabs.close')}
            className="grid h-5 w-5 place-items-center rounded text-muted/60 transition hover:bg-danger/15 hover:text-danger"
          >
            <X size={12} />
          </button>
        </div>
      ))}

      <button
        type="button"
        onClick={() => setActiveTab('home')}
        aria-label={t('tabs.new')}
        title={t('tabs.new')}
        className="grid h-7 w-7 shrink-0 place-items-center rounded-lg text-muted transition hover:bg-panel hover:text-ink"
      >
        <Plus size={15} />
      </button>
    </div>
  )
}
