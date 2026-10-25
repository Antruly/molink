import { useCallback, useEffect, useMemo, useState } from 'react'
import {
  FolderUp,
  LayoutGrid,
  List,
  MessageSquare,
  Monitor,
  Star,
  Trash2,
} from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { lookupDevices, type PeerStatus } from '../../lib/api'
import { platformEmoji } from '../../lib/avatar'
import { formatRelative } from '../../lib/format'
import { useAppStore, type SessionIntent } from '../../store/app'
import { Avatar } from './Avatar'

export interface DeviceItem {
  key: string
  id?: string
  host: string
  port: number
  label: string
  platform?: string
  avatar?: string
  online?: boolean
  source: 'recent' | 'saved' | 'lan'
  token?: string
  lastSeen: number
}

type Tab = 'recent' | 'saved' | 'lan' | 'online'

export function DeviceList({
  mode,
  onPick,
  onConnect,
}: {
  mode: SessionIntent
  /// Single click: fill the connect form instead of starting a session.
  onPick: (item: DeviceItem) => void
  /// Double click or an explicit mode button: connect now.
  onConnect: (item: DeviceItem, intent: SessionIntent) => void
}) {
  const { t } = useTranslation()
  const recents = useAppStore((state) => state.recents)
  const savedDevices = useAppStore((state) => state.savedDevices)
  const nearby = useAppStore((state) => state.nearby)
  const localMode = useAppStore((state) => state.localMode)
  const server = useAppStore((state) => state.server)
  const token = useAppStore((state) => state.token)
  const forgetRecent = useAppStore((state) => state.forgetRecent)
  const refreshDiscovery = useAppStore((state) => state.refreshDiscovery)
  const saveDevice = useAppStore((state) => state.saveDevice)
  const removeSavedDevice = useAppStore((state) => state.removeSavedDevice)
  const patchSavedDevice = useAppStore((state) => state.patchSavedDevice)
  const historyView = useAppStore((state) => state.historyView)
  const setHistoryView = useAppStore((state) => state.setHistoryView)

  const [tab, setTab] = useState<Tab>('recent')
  const [peers, setPeers] = useState<Record<string, PeerStatus>>({})
  const [nowTick, setNowTick] = useState(() => Date.now())

  useEffect(() => {
    const timer = window.setInterval(() => setNowTick(Date.now()), 15000)
    return () => window.clearInterval(timer)
  }, [])

  const serverInfo = server?.server
  const serverUrl = serverInfo?.url ?? ''
  const serverEnabled = serverInfo?.enabled ?? true

  useEffect(() => {
    if (!localMode) return
    void refreshDiscovery()
    const timer = window.setInterval(() => void refreshDiscovery(), 3000)
    return () => window.clearInterval(timer)
  }, [localMode, refreshDiscovery])

  const trackedIds = useMemo(() => {
    const ids = new Set<string>()
    for (const device of savedDevices) {
      if (device.id) ids.add(device.id)
    }
    for (const recent of recents) {
      if (recent.id) ids.add(recent.id)
    }
    return Array.from(ids).slice(0, 20)
  }, [savedDevices, recents])

  const refreshPeers = useCallback(async () => {
    if (!serverEnabled || !serverUrl || trackedIds.length === 0) {
      setPeers({})
      return
    }
    try {
      const list = await lookupDevices(serverUrl, token, trackedIds)
      const next: Record<string, PeerStatus> = {}
      for (const peer of list) {
        next[peer.id] = peer
        patchSavedDevice(peer.id, {
          label: peer.name,
          platform: peer.platform,
          avatar: peer.avatar,
        })
      }
      setPeers(next)
    } catch {
      // The directory may be offline; keep the previous statuses.
    }
  }, [serverEnabled, serverUrl, trackedIds, patchSavedDevice])

  useEffect(() => {
    const initial = window.setTimeout(() => void refreshPeers(), 0)
    const timer = window.setInterval(() => void refreshPeers(), 30000)
    return () => {
      window.clearTimeout(initial)
      window.clearInterval(timer)
    }
  }, [refreshPeers])

  const recentItems = useMemo<DeviceItem[]>(
    () =>
      recents.map((recent) => {
        const peer = recent.id ? peers[recent.id] : undefined
        return {
          key: `recent:${recent.host}:${recent.port}`,
          id: recent.id,
          host: recent.host,
          port: recent.port,
          label: peer?.name || recent.label,
          platform: peer?.platform || recent.platform,
          avatar: peer?.avatar || recent.avatar,
          online: peer?.online,
          source: 'recent',
          token: recent.token,
          lastSeen: recent.at,
        }
      }),
    [recents, peers],
  )

  const savedItems = useMemo<DeviceItem[]>(
    () =>
      savedDevices.map((device) => {
        const peer = device.id ? peers[device.id] : undefined
        return {
          key: device.key,
          id: device.id,
          host: device.host,
          port: device.port,
          label: peer?.name || device.label,
          platform: peer?.platform || device.platform,
          avatar: peer?.avatar || device.avatar,
          online: peer?.online,
          source: 'saved',
          token: device.token,
          lastSeen: device.savedAt,
        }
      }),
    [savedDevices, peers],
  )

  const lanItems = useMemo<DeviceItem[]>(
    () =>
      nearby.map((peer) => ({
        key: `lan:${peer.device_id}`,
        id: peer.device_id,
        host: peer.host,
        port: peer.port,
        label: peer.name || peer.device_id,
        platform: peer.platform,
        online: nowTick - peer.last_seen_ms < 30000,
        source: 'lan',
        lastSeen: peer.last_seen_ms,
      })),
    [nearby, nowTick],
  )

  const onlineItems = useMemo<DeviceItem[]>(() => {
    const byId = new Map<string, DeviceItem>()
    for (const item of [...savedItems, ...recentItems]) {
      if (!item.id || byId.has(item.id)) continue
      const peer = peers[item.id]
      byId.set(item.id, {
        ...item,
        label: peer?.name || item.label,
        platform: peer?.platform || item.platform,
        avatar: peer?.avatar || item.avatar,
        online: peer?.online,
      })
    }
    return Array.from(byId.values()).sort((a, b) => {
      if (Boolean(a.online) !== Boolean(b.online)) return a.online ? -1 : 1
      return a.label.localeCompare(b.label)
    })
  }, [savedItems, recentItems, peers])

  const items =
    tab === 'recent'
      ? recentItems
      : tab === 'saved'
        ? savedItems
        : tab === 'lan'
          ? lanItems
          : onlineItems

  const isSaved = (item: DeviceItem) =>
    savedDevices.some(
      (device) =>
        (item.id && device.id === item.id) ||
        (device.host === item.host && device.port === item.port),
    )

  const toggleSaved = (item: DeviceItem) => {
    const existing = savedDevices.find(
      (device) =>
        (item.id && device.id === item.id) ||
        (device.host === item.host && device.port === item.port),
    )
    if (existing) {
      removeSavedDevice(existing.key)
      return
    }
    saveDevice({
      id: item.id,
      host: item.host,
      port: item.port,
      label: item.label,
      platform: item.platform,
      avatar: item.avatar,
      token: item.token,
    })
  }

  const onlineDot = (item: DeviceItem) => {
    if (item.online === undefined) {
      return <span className="h-2 w-2 rounded-full bg-muted/40" title={t('simple.statusUnknown')} />
    }
    return (
      <span
        className={
          'h-2 w-2 rounded-full ' +
          (item.online ? 'bg-accent shadow-[0_0_6px_var(--accent)]' : 'bg-muted/40')
        }
        title={item.online ? t('simple.online') : t('simple.offline')}
      />
    )
  }

  const quickActions = (item: DeviceItem) => (
    <div className="flex items-center gap-0.5">
      {(
        [
          ['desktop', Monitor],
          ['files', FolderUp],
          ['chat', MessageSquare],
        ] as const
      ).map(([intent, Icon]) => (
        <button
          key={intent}
          type="button"
          className="grid h-7 w-7 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink"
          title={t(`simple.mode${intent[0].toUpperCase()}${intent.slice(1)}`)}
          onClick={(event) => {
            event.stopPropagation()
            onConnect(item, intent)
          }}
        >
          <Icon size={13} />
        </button>
      ))}
    </div>
  )

  const forgetButton = (item: DeviceItem) => {
    if (item.source !== 'recent') return null
    return (
      <button
        type="button"
        className="grid h-7 w-7 place-items-center rounded-md text-muted transition hover:bg-danger/10 hover:text-danger"
        title={t('home.forget')}
        onClick={(event) => {
          event.stopPropagation()
          forgetRecent(item.lastSeen)
        }}
      >
        <Trash2 size={13} />
      </button>
    )
  }

  const starButton = (item: DeviceItem) => {
    const saved = isSaved(item)
    return (
      <button
        type="button"
        className={
          'grid h-7 w-7 place-items-center rounded-md transition hover:bg-panel ' +
          (saved ? 'text-warn' : 'text-muted hover:text-ink')
        }
        title={saved ? t('simple.unsave') : t('simple.save')}
        onClick={(event) => {
          event.stopPropagation()
          toggleSaved(item)
        }}
      >
        <Star size={13} fill={saved ? 'currentColor' : 'none'} />
      </button>
    )
  }

  const tabs: Array<{ value: Tab; label: string; count?: number }> = [
    { value: 'recent', label: t('simple.recent'), count: recentItems.length },
    { value: 'saved', label: t('simple.saved'), count: savedItems.length },
    { value: 'lan', label: t('simple.lan'), count: lanItems.length },
    { value: 'online', label: t('simple.onlineDevices'), count: onlineItems.length },
  ]

  const emptyText =
    tab === 'recent'
      ? t('simple.noRecent')
      : tab === 'saved'
        ? t('simple.noSaved')
        : tab === 'lan'
          ? localMode
            ? t('simple.noLan')
            : t('simple.lanLocalOnly')
          : !serverEnabled
            ? t('simple.serverDisabledHint')
            : t('simple.noOnline')

  return (
    <section className="card flex h-full min-h-[240px] flex-col p-0">
      <div className="flex flex-wrap items-center gap-2 border-b border-line-soft p-2.5">
        <div className="flex flex-wrap items-center gap-0.5">
          {tabs.map((item) => (
            <button
              key={item.value}
              type="button"
              className={
                'rounded-full px-2.5 py-1 text-[11px] transition ' +
                (tab === item.value
                  ? 'bg-accent-soft text-accent'
                  : 'text-muted hover:bg-panel hover:text-ink')
              }
              onClick={() => setTab(item.value)}
            >
              {item.label}
              {typeof item.count === 'number' && item.count > 0 ? (
                <span className="ml-1 text-[9px] opacity-70">{item.count}</span>
              ) : null}
            </button>
          ))}
        </div>
        <div className="ml-auto flex items-center gap-1">
          {tab === 'recent' && recents.length > 0 ? (
            <button
              type="button"
              className="grid h-7 w-7 place-items-center rounded-md text-muted transition hover:bg-danger/10 hover:text-danger"
              title={t('simple.clearRecent')}
              onClick={() => {
                if (!window.confirm(t('simple.clearRecentConfirm'))) return
                for (const recent of [...recents]) forgetRecent(recent.at)
              }}
            >
              <Trash2 size={13} />
            </button>
          ) : null}
          <button
            type="button"
            className={
              'grid h-7 w-7 place-items-center rounded-md transition ' +
              (historyView === 'cards'
                ? 'bg-panel text-ink'
                : 'text-muted hover:text-ink')
            }
            onClick={() => setHistoryView('cards')}
            title={t('simple.cards')}
          >
            <LayoutGrid size={13} />
          </button>
          <button
            type="button"
            className={
              'grid h-7 w-7 place-items-center rounded-md transition ' +
              (historyView === 'list'
                ? 'bg-panel text-ink'
                : 'text-muted hover:text-ink')
            }
            onClick={() => setHistoryView('list')}
            title={t('simple.list')}
          >
            <List size={13} />
          </button>
        </div>
      </div>

      <div className="min-h-0 flex-1 overflow-y-auto p-2.5">
        {items.length === 0 ? (
          <div className="rounded-xl border border-dashed border-line px-4 py-8 text-center text-[12px] leading-relaxed text-muted">
            {emptyText}
          </div>
        ) : historyView === 'cards' ? (
          <div className="grid gap-2 sm:grid-cols-2 2xl:grid-cols-3">
            {items.map((item) => (
              <div
                key={item.key}
                role="button"
                tabIndex={0}
                className="group cursor-pointer rounded-xl border border-line bg-panel/35 p-3 transition hover:border-accent/40 hover:bg-panel/60"
                title={t('simple.clickToFill')}
                onClick={() => onPick(item)}
                onDoubleClick={() => onConnect(item, mode)}
                onKeyDown={(event) => {
                  if (event.key === 'Enter' || event.key === ' ') {
                    event.preventDefault()
                    onPick(item)
                  }
                }}
              >
                <div className="flex items-start gap-2.5">
                  <Avatar
                    value={item.avatar}
                    name={item.label}
                    platform={item.platform}
                    size={40}
                  />
                  <div className="min-w-0 flex-1">
                    <div className="flex items-center gap-1.5">
                      {onlineDot(item)}
                      <span className="truncate text-[13px] font-medium text-ink">
                        {item.label}
                      </span>
                    </div>
                    <div className="mt-0.5 truncate font-mono text-[10px] text-muted">
                      {item.id ? item.id : `${item.host}:${item.port}`}
                    </div>
                    <div className="mt-0.5 flex items-center gap-1 text-[10px] text-muted">
                      <span>{platformEmoji(item.platform)}</span>
                      <span className="truncate">
                        {item.source === 'lan'
                          ? `${item.host}:${item.port}`
                          : item.id
                            ? t('simple.viaDirectory')
                            : `${item.host}:${item.port}`}
                      </span>
                    </div>
                  </div>
                  <div className="flex items-center opacity-0 transition group-hover:opacity-100">
                    {forgetButton(item)}
                    {starButton(item)}
                  </div>
                </div>
                <div className="mt-2 flex items-center justify-between">
                  <span className="text-[10px] text-muted">
                    {item.lastSeen ? formatRelative(item.lastSeen) : ''}
                  </span>
                  <div className="flex items-center">
                    {quickActions(item)}
                    <span className="ml-1 opacity-0 transition group-hover:opacity-100">
                      {forgetButton(item)}
                      {starButton(item)}
                    </span>
                  </div>
                </div>
              </div>
            ))}
          </div>
        ) : (
          <div className="overflow-x-auto">
            <table className="data-table">
              <thead>
                <tr>
                  <th>{t('simple.device')}</th>
                  <th>{t('simple.address')}</th>
                  <th>{t('simple.lastSeen')}</th>
                  <th className="text-right">{t('simple.actions')}</th>
                </tr>
              </thead>
              <tbody>
                {items.map((item) => (
                  <tr
                    key={item.key}
                    className="cursor-pointer"
                    title={t('simple.clickToFill')}
                    onClick={() => onPick(item)}
                    onDoubleClick={() => onConnect(item, mode)}
                  >
                    <td>
                      <div className="flex items-center gap-2">
                        <Avatar
                          value={item.avatar}
                          name={item.label}
                          platform={item.platform}
                          size={30}
                        />
                        <div className="min-w-0">
                          <div className="flex items-center gap-1.5">
                            {onlineDot(item)}
                            <span className="truncate font-medium text-ink">{item.label}</span>
                          </div>
                          <div className="font-mono text-[10px] text-muted">
                            {item.id || item.host}
                          </div>
                        </div>
                      </div>
                    </td>
                    <td className="font-mono text-[11px] text-muted">
                      {item.id ? t('simple.viaDirectory') : `${item.host}:${item.port}`}
                    </td>
                    <td className="whitespace-nowrap text-muted">
                      {item.lastSeen ? formatRelative(item.lastSeen) : '—'}
                    </td>
                    <td>
                      <div className="flex justify-end gap-0.5">
                        {quickActions(item)}
                        {forgetButton(item)}
                        {starButton(item)}
                      </div>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </div>
    </section>
  )
}
