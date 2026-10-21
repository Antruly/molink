import { create } from 'zustand'
import { fetchDeviceInfo, type DeviceInfo } from '../lib/api'
import type {
  ClientPhase,
  CursorPosition,
  CursorShape,
  Welcome,
} from '../lib/client'
import type { Frame } from '../lib/protocol'
import {
  SessionConnection,
  type Stats,
  type TabPatch,
} from '../lib/session-connection'

export type Theme = 'light' | 'dark'

export interface SessionTab {
  id: string
  label: string
  host: string
  port: number
  token: string
  phase: ClientPhase
  welcome: Welcome | null
  frame: Frame | null
  cursorShape: CursorShape | null
  cursorPosition: CursorPosition | null
  stats: Stats
  error: string | null
  controlEnabled: boolean
}

export interface RecentConnection {
  host: string
  port: number
  label: string
  at: number
}

interface AppState {
  theme: Theme
  qualityWatermark: boolean
  settingsOpen: boolean
  device: DeviceInfo | null
  deviceError: string | null
  deviceLoading: boolean
  tabs: SessionTab[]
  activeTabId: string
  recents: RecentConnection[]

  setTheme: (theme: Theme) => void
  toggleTheme: () => void
  setQualityWatermark: (enabled: boolean) => void
  setSettingsOpen: (open: boolean) => void
  refreshDevice: () => Promise<void>
  createSession: (
    host: string,
    port: number,
    token: string,
    label?: string,
  ) => string
  closeTab: (id: string) => void
  setActiveTab: (id: string) => void
  setControlEnabled: (id: string, enabled: boolean) => void
  forgetRecent: (at: number) => void
}

const THEME_KEY = 'molink.theme'
const QUALITY_KEY = 'molink.quality'
const RECENT_KEY = 'molink.recents'
const EMPTY_STATS: Stats = {
  fps: 0,
  latencyMs: null,
  bytesPerSec: 0,
  frames: 0,
}

function readStorage(key: string): string {
  try {
    return window.localStorage.getItem(key) ?? ''
  } catch {
    return ''
  }
}

function writeStorage(key: string, value: string): void {
  try {
    window.localStorage.setItem(key, value)
  } catch {
    // Private windows and blocked storage are fine.
  }
}

function initialTheme(): Theme {
  const saved = readStorage(THEME_KEY)
  if (saved === 'light' || saved === 'dark') return saved
  return window.matchMedia?.('(prefers-color-scheme: light)').matches
    ? 'light'
    : 'dark'
}

function initialRecents(): RecentConnection[] {
  try {
    const parsed = JSON.parse(readStorage(RECENT_KEY)) as RecentConnection[]
    if (!Array.isArray(parsed)) return []
    return parsed
      .filter(
        (item) =>
          typeof item?.host === 'string' &&
          typeof item?.port === 'number' &&
          typeof item?.label === 'string',
      )
      .slice(0, 8)
  } catch {
    return []
  }
}

const runtimes = new Map<string, SessionConnection>()
let nextId = 1

export function sessionConnection(id: string): SessionConnection | undefined {
  return runtimes.get(id)
}

export const useAppStore = create<AppState>((set, get) => {
  const patchTab = (id: string, patch: TabPatch): void => {
    set((state) => ({
      tabs: state.tabs.map((tab) =>
        tab.id === id ? { ...tab, ...patch } : tab,
      ),
    }))
  }

  const remember = (host: string, port: number, label: string): void => {
    const recent: RecentConnection = { host, port, label, at: Date.now() }
    const recents = [
      recent,
      ...get().recents.filter(
        (item) => !(item.host === host && item.port === port),
      ),
    ].slice(0, 8)
    writeStorage(RECENT_KEY, JSON.stringify(recents))
    set({ recents })
  }

  return {
    theme: initialTheme(),
    qualityWatermark: readStorage(QUALITY_KEY) === '1',
    settingsOpen: false,
    device: null,
    deviceError: null,
    deviceLoading: false,
    tabs: [],
    activeTabId: 'home',
    recents: initialRecents(),

    setTheme: (theme) => {
      writeStorage(THEME_KEY, theme)
      set({ theme })
    },

    toggleTheme: () => {
      const theme = get().theme === 'dark' ? 'light' : 'dark'
      writeStorage(THEME_KEY, theme)
      set({ theme })
    },

    setQualityWatermark: (enabled) => {
      writeStorage(QUALITY_KEY, enabled ? '1' : '0')
      set({ qualityWatermark: enabled })
    },

    setSettingsOpen: (open) => set({ settingsOpen: open }),

    refreshDevice: async () => {
      set({ deviceLoading: true })
      try {
        const device = await fetchDeviceInfo()
        set({ device, deviceError: null, deviceLoading: false })
      } catch (error) {
        set({
          deviceError: error instanceof Error ? error.message : 'unreachable',
          deviceLoading: false,
        })
      }
    },

    createSession: (host, port, token, label) => {
      const id = `session-${nextId++}`
      const trimmedToken = token.trim()
      const tab: SessionTab = {
        id,
        label: label?.trim() || host,
        host: host.trim(),
        port,
        token: trimmedToken,
        phase: 'connecting',
        welcome: null,
        frame: null,
        cursorShape: null,
        cursorPosition: null,
        stats: EMPTY_STATS,
        error: null,
        controlEnabled: false,
      }
      set((state) => ({
        tabs: [...state.tabs, tab],
        activeTabId: id,
      }))

      const connection = new SessionConnection({
        onPatch: (patch) => patchTab(id, patch),
        onClipboard: (text) => {
          if (!navigator.clipboard?.writeText) {
            patchTab(id, { error: 'clipboard_unavailable' })
            return
          }
          navigator.clipboard.writeText(text).catch(() => {
            patchTab(id, { error: 'clipboard_write_failed' })
          })
        },
      })
      runtimes.set(id, connection)

      if (trimmedToken === '') {
        patchTab(id, { phase: 'error', error: 'no_token' })
      } else {
        connection.connect(tab.host, port, trimmedToken)
        remember(tab.host, port, tab.label)
      }
      return id
    },

    closeTab: (id) => {
      const connection = runtimes.get(id)
      connection?.disconnect()
      runtimes.delete(id)
      set((state) => {
        const index = state.tabs.findIndex((tab) => tab.id === id)
        const tabs = state.tabs.filter((tab) => tab.id !== id)
        let activeTabId = state.activeTabId
        if (activeTabId === id) {
          activeTabId = tabs.length
            ? tabs[Math.max(0, Math.min(index - 1, tabs.length - 1))]!.id
            : 'home'
        }
        return { tabs, activeTabId }
      })
    },

    setActiveTab: (id) => set({ activeTabId: id }),

    setControlEnabled: (id, enabled) =>
      patchTab(id, { controlEnabled: enabled }),

    forgetRecent: (at) => {
      const recents = get().recents.filter((item) => item.at !== at)
      writeStorage(RECENT_KEY, JSON.stringify(recents))
      set({ recents })
    },
  }
})
