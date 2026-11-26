import { House, Maximize2, Minus, Plus, ShieldCheck, ShieldOff, X } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { openSessionWindow } from '../lib/windows'
import { formatDisplayId } from '../lib/format'
import { intentSuffixKey, tabTitle } from '../lib/session-intent'
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

function SecurityIcon({ tab }: { tab: SessionTab }) {
  const { t } = useTranslation()
  const security = tab.welcome?.security
  if (!security) return null
  if (security === 'e2e') {
    return (
      <span
        className="shrink-0 text-sky-500"
        title={t('session.secureE2e')}
        aria-label={t('session.secureE2e')}
      >
        <ShieldCheck size={12} />
      </span>
    )
  }
  if (security === 'relay') {
    return (
      <span
        className="shrink-0 text-amber-500"
        title={t('session.secureRelay')}
        aria-label={t('session.secureRelay')}
      >
        <ShieldCheck size={12} />
      </span>
    )
  }
  return (
    <span
      className="shrink-0 text-muted/70"
      title={t('session.insecure')}
      aria-label={t('session.insecure')}
    >
      <ShieldOff size={12} />
    </span>
  )
}

export function TabBar({ inline = false }: { inline?: boolean }) {
  const { t } = useTranslation()
  const tabs = useAppStore((state) => state.tabs)
  const activeTabId = useAppStore((state) => state.activeTabId)
  const setActiveTab = useAppStore((state) => state.setActiveTab)
  const closeTab = useAppStore((state) => state.closeTab)
  const setTabMinimized = useAppStore((state) => state.setTabMinimized)
  const localMode = useAppStore((state) => state.localMode)
  // `YQYX-OFFICE01 · 文件` for a files tab; the bare label for desktop.
  const labelFor = (tab: SessionTab): string => {
    const key = intentSuffixKey(tab.intent)
    return tabTitle(tab.label, tab.intent, key ? String(t(key)) : '')
  }

  return (
    <div
      className={
        inline
          ? 'flex min-w-0 flex-1 items-center gap-1 overflow-x-auto'
          : 'flex h-11 shrink-0 items-center gap-1 overflow-x-auto border-b border-line-soft bg-surface/40 px-2'
      }
    >
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
                intent: tab.intent,
              })
            ) {
              closeTab(tab.id)
            }
          }}
          className={
            'group flex shrink-0 items-center gap-1.5 rounded-lg pl-3 pr-1.5 text-[12px] transition ' +
            (tab.minimized
              ? 'border border-dashed border-line bg-panel/30 opacity-70 '
              : '') +
            (activeTabId === tab.id && !tab.minimized
              ? 'bg-accent-soft text-accent'
              : 'text-muted hover:bg-panel hover:text-ink')
          }
        >
          <button
            type="button"
            onClick={() => setActiveTab(tab.id)}
            className="flex max-w-[190px] items-center gap-2 py-1.5"
            title={
              tab.deviceId
                ? `${formatDisplayId(tab.deviceId)} · ${labelFor(tab)}`
                : `${tab.host}:${tab.port}`
            }
          >
            <StatusDot tab={tab} />
            <SecurityIcon tab={tab} />
            <span className="truncate">{labelFor(tab)}</span>
          </button>
          <button
            type="button"
            onClick={() => setTabMinimized(tab.id, !tab.minimized)}
            aria-label={tab.minimized ? t('tabs.restore') : t('tabs.minimize')}
            title={tab.minimized ? t('tabs.restore') : t('tabs.minimize')}
            aria-pressed={tab.minimized === true}
            className="grid h-5 w-5 place-items-center rounded text-muted/60 transition hover:bg-panel hover:text-ink"
          >
            {tab.minimized ? <Maximize2 size={11} /> : <Minus size={12} />}
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
