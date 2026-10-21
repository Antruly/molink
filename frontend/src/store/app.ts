import { create } from 'zustand'
import {
  ApiError,
  fetchDeviceInfo,
  fetchInfo,
  fetchState,
  login as apiLogin,
  postActivity,
  postLockTimeout,
  postUiState,
  type DeviceInfo,
  type ServerState,
} from '../lib/api'
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
export type AuthPhase = 'booting' | 'login' | 'locked' | 'ready'

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
  authPhase: AuthPhase
  localMode: boolean
  token: string
  locked: boolean
  lockTimeout: number
  server: ServerState | null
  stateError: string | null
  device: DeviceInfo | null
  tabs: SessionTab[]
  activeTabId: string
  recents: RecentConnection[]

  setTheme: (theme: Theme) => void
  toggleTheme: () => void
  setQualityWatermark: (enabled: boolean) => void
  setSettingsOpen: (open: boolean) => void
  boot: () => Promise<void>
  refreshState: () => Promise<void>
  authenticate: (code: string) => Promise<boolean>
  setLockTimeout: (seconds: number) => Promise<void>
  touchActivity: () => void
  syncUiState: () => void
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

const UI_CLIENT_ID = (() => {
  try {
    const existing = window.sessionStorage.getItem('molink.clientId')
    if (existing) return existing
    const created = crypto.randomUUID()
    window.sessionStorage.setItem('molink.clientId', created)
    return created
  } catch {
    return 'default'
  }
})()

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
    authPhase: 'booting',
    localMode: false,
    token: '',
    locked: false,
    lockTimeout: 60,
    server: null,
    stateError: null,
    device: null,
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

    boot: async () => {
      set({ authPhase: 'booting', stateError: null })
      try {
        const state = await fetchState()
        set({
          server: state,
          localMode: state.local,
          locked: state.locked,
          lockTimeout: state.lock_timeout,
        })
        if (state.locked) {
          set({ authPhase: 'locked' })
          return
        }
        if (state.local) {
          try {
            const device = await fetchDeviceInfo()
            set({
              device,
              token: device.token,
              authPhase: 'ready',
              stateError: null,
            })
            get().syncUiState()
            return
          } catch {
            set({ authPhase: 'login', stateError: 'local_auth_failed' })
            return
          }
        }
        set({ authPhase: 'login' })
      } catch (error) {
        set({
          authPhase: 'login',
          stateError:
            error instanceof Error ? error.message : 'core_unreachable',
        })
      }
    },

    refreshState: async () => {
      try {
        const state = await fetchState()
        const previous = get()
        set({
          server: state,
          localMode: state.local,
          locked: state.locked,
          lockTimeout: state.lock_timeout,
        })
        if (state.locked) {
          if (previous.authPhase !== 'locked') {
            for (const id of Array.from(runtimes.keys())) {
              runtimes.get(id)?.disconnect()
              runtimes.delete(id)
            }
            set({
              authPhase: 'locked',
              token: '',
              tabs: [],
              activeTabId: 'home',
              device: previous.device
                ? { ...previous.device, token: '' }
                : null,
            })
          }
          return
        }
        if (previous.authPhase === 'locked') return
        if (
          state.local &&
          previous.authPhase === 'login' &&
          previous.token === ''
        ) {
          try {
            const device = await fetchDeviceInfo()
            set({ device, token: device.token, authPhase: 'ready' })
            get().syncUiState()
          } catch {
            // stay on the login screen
          }
        }
      } catch (error) {
        set({
          stateError:
            error instanceof Error ? error.message : 'core_unreachable',
        })
      }
    },

    authenticate: async (code) => {
      try {
        const result = await apiLogin(code)
        let capabilities = {
          screen_capture: false,
          input_injection: false,
          clipboard: false,
        }
        try {
          const info = await fetchInfo(code)
          capabilities = info.capabilities
        } catch {
          // capabilities are cosmetic; login already succeeded
        }
        const state = get().server
        const host =
          state?.host && state.host !== '0.0.0.0'
            ? state.host
            : window.location.hostname
        const port = state?.port || Number(window.location.port) || 21118
        set({
          token: code,
          locked: false,
          localMode: result.local,
          lockTimeout: result.lock_timeout,
          authPhase: 'ready',
          stateError: null,
          device: {
            device_id: result.device_id,
            token: code,
            service: result.service,
            product: 'moLink',
            version: result.version,
            protocol: result.protocol,
            host,
            port,
            capabilities,
          },
        })
        get().syncUiState()
        return true
      } catch (error) {
        let code = 'core_unreachable'
        if (error instanceof ApiError) {
          if (error.status === 401) code = 'invalid_token'
          else if (error.status === 423) code = 'locked'
        }
        set({ stateError: code })
        return false
      }
    },

    setLockTimeout: async (seconds) => {
      const token = get().token
      if (!token) return
      try {
        const value = await postLockTimeout(token, seconds)
        set({ lockTimeout: value })
      } catch {
        set({ stateError: 'settings_failed' })
      }
    },

    touchActivity: () => {
      const token = get().token
      if (!token) return
      void postActivity(token).catch(() => undefined)
    },

    syncUiState: () => {
      const token = get().token
      if (!token) return
      void postUiState(token, UI_CLIENT_ID, get().tabs.length).catch(
        () => undefined,
      )
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
      get().syncUiState()

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
      get().syncUiState()
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
