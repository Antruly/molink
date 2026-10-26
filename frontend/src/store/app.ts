import { create } from 'zustand'
import {
  ApiError,
  fetchDeviceInfo,
  fetchDiscovery,
  fetchDomainDevices,
  fetchInfo,
  fetchSettings,
  fetchState,
  fetchUiPreferences,
  postDomain,
  saveAvatar,
  saveUiPreferences,
  postServerEnabled,
  type DomainDevice,
  login as apiLogin,
  postAccessDecision,
  postActivity,
  postAuthCode,
  postLockTimeout,
  postLongTermCode,
  postNetworkPorts,
  postSettings,
  postUiState,
  type AuthCode,
  type DeviceInfo,
  type DiscoveredPeer,
  type SecuritySettings,
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
import {
  setWindowFullscreen as setNativeFullscreen,
  shellInfo,
} from '../lib/shell'

export type Theme = 'light' | 'dark'
export type AuthPhase = 'booting' | 'login' | 'locked' | 'ready'
/// "auto" is resolved in the client into a concrete preset plus a frame rate.
/// ultra/lossless use the raw BGRA preset (15/30 fps); game is the lossy
/// frame-rate-first preset (60 fps).
export type QualityMode =
  | 'turbo'
  | 'smooth'
  | 'balanced'
  | 'quality'
  | 'ultra'
  | 'lossless'
  | 'game'
  | 'auto'
/// A control session starts with input enabled; a view-only session starts
/// with it off. The session toolbar can still change it later.
export type SessionMode = 'control' | 'view'
export type SessionIntent = 'desktop' | 'files' | 'chat'
export type UiMode = 'simple' | 'advanced'
/// 'unknown_domain' means the code was revoked (the device left the group).
export type DomainOutcome = 'ok' | 'unknown_domain' | 'failed'
export type HistoryView = 'cards' | 'list'

export interface SessionTab {
  id: string
  label: string
  host: string
  port: number
  token: string
  /// Directory device ID; the connection then tries ICE through the local
  /// core before falling back to the direct address.
  deviceId?: string
  phase: ClientPhase
  welcome: Welcome | null
  frame: Frame | null
  cursorShape: CursorShape | null
  cursorPosition: CursorPosition | null
  stats: Stats
  error: string | null
  /// Machine-readable error code from the host (e.g. unauthorized).
  errorCode?: string | null
  /// "relay" or "p2p" for a device-ID session.
  transport?: string
  /// The client core is decoding AV1 patches for this session.
  av1Active?: boolean
  av1WireBytes?: number
  av1DecodedBytes?: number
  notice: string | null
  /// When the host must approve this session: epoch ms of the 60 s deadline.
  approvalDeadline: number | null
  /// Epoch ms of the next automatic reconnect attempt after a disconnect.
  retryAt: number | null
  controlEnabled: boolean
  mode: SessionMode
  /// What the simple home asked for before the session started.
  intent: SessionIntent
  clipboardEnabled: boolean
  displayIndex: number
}

export interface RecentConnection {
  host: string
  port: number
  label: string
  at: number
  /// Optional saved authorization code. Stored only when the user opts in.
  token?: string
  id?: string
  platform?: string
  avatar?: string
}

export interface SavedDevice {
  key: string
  id?: string
  host: string
  port: number
  label: string
  platform?: string
  avatar?: string
  token?: string
  savedAt: number
}

/// The last values typed into the simple connect form. The shell opens a new
/// ephemeral port on every launch, so localStorage alone would lose them; the
/// same object is also mirrored into the core's ui.json.
export interface ConnectDraft {
  input: string
  code: string
}

interface AppState {
  theme: Theme
  qualityWatermark: boolean
  qualityMode: QualityMode
  settingsOpen: boolean
  /// The native shell window is fullscreen but the app chrome stays visible.
  /// This is the title bar's fullscreen control, not session immersive mode.
  windowFullscreen: boolean
  /// The native shell window is fullscreen and the app chrome is hidden.
  immersive: boolean
  historyView: HistoryView
  preferredMode: SessionMode
  /// Force every device-ID connection through the server relay (no ICE).
  forceRelay: boolean
  uiMode: UiMode
  savedDevices: SavedDevice[]
  authPhase: AuthPhase
  localMode: boolean
  token: string
  locked: boolean
  lockTimeout: number
  server: ServerState | null
  stateError: string | null
  device: DeviceInfo | null
  security: SecuritySettings | null
  nearby: DiscoveredPeer[]
  tabs: SessionTab[]
  activeTabId: string
  recents: RecentConnection[]
  connectDraft: ConnectDraft

  setTheme: (theme: Theme) => void
  toggleTheme: () => void
  setQualityWatermark: (enabled: boolean) => void
  setQualityMode: (mode: QualityMode) => void
  setSettingsOpen: (open: boolean) => void
  setWindowFullscreen: (value: boolean) => void
  setImmersive: (value: boolean) => void
  setHistoryView: (view: HistoryView) => void
  setPreferredMode: (mode: SessionMode) => void
  setForceRelay: (value: boolean) => void
  setUiMode: (mode: UiMode) => void
  setConnectDraft: (patch: Partial<ConnectDraft>) => void
  loadUiPreferences: () => Promise<void>
  saveDevice: (device: Omit<SavedDevice, 'key' | 'savedAt'> & { key?: string }) => void
  patchSavedDevice: (id: string, patch: Partial<SavedDevice>) => void
  removeSavedDevice: (key: string) => void
  setServerEnabled: (enabled: boolean) => Promise<boolean>
  setAvatarValue: (avatar: string) => Promise<boolean>
  /// Join another device's code, or ask the directory for a fresh one.
  setDomain: (action: 'join' | 'rotate', code?: string) => Promise<DomainOutcome>
  refreshDomainDevices: () => Promise<DomainDevice[]>
  boot: () => Promise<void>
  refreshState: () => Promise<void>
  authenticate: (code: string) => Promise<boolean>
  setLockTimeout: (seconds: number) => Promise<void>
  setNetworkPorts: (tcp: number, udp: number) => Promise<boolean>
  setAuthCode: (
    mode: 'single' | 'multi',
    uses: number,
    value?: string,
  ) => Promise<AuthCode | null>
  refreshSecurity: () => Promise<void>
  refreshDiscovery: () => Promise<void>
  refreshDevice: () => Promise<void>
  saveSecurity: (values: {
    allowRemote: boolean
    allowWeb: boolean
    discovery: boolean
    allowAccessRequests: boolean
    encryptRemote?: boolean
    encryptLan?: boolean
    name: string
    stunServer?: string
  }) => Promise<boolean>
  decideAccessRequest: (id: number, allow: boolean) => Promise<void>
  setLongTermCode: (value: string) => Promise<boolean>
  touchActivity: () => void
  syncUiState: () => void
  rememberRecent: (
    host: string,
    port: number,
    label: string,
    token?: string,
    saveToken?: boolean,
  ) => void
  createSession: (
    host: string,
    port: number,
    token: string,
    label?: string,
    saveToken?: boolean,
    requestAccess?: boolean,
    mode?: SessionMode,
    intent?: SessionIntent,
    deviceId?: string,
    platform?: string,
    avatar?: string,
  ) => string
  closeTab: (id: string) => void
  setActiveTab: (id: string) => void
  setControlEnabled: (id: string, enabled: boolean) => void
  setClipboardEnabled: (id: string, enabled: boolean) => void
  forgetRecent: (at: number) => void
  relabelRecent: (host: string, port: number, label: string) => void
}

const THEME_KEY = 'molink.theme'
const QUALITY_KEY = 'molink.quality'
const QUALITY_MODE_KEY = 'molink.qualityMode'
const RECENT_KEY = 'molink.recents'
const HISTORY_VIEW_KEY = 'molink.historyView'
const MODE_KEY = 'molink.sessionMode'
const UI_MODE_KEY = 'molink.uiMode'
const SAVED_DEVICES_KEY = 'molink.savedDevices'
const CONNECT_DRAFT_KEY = 'molink.connectDraft'
const EMPTY_STATS: Stats = {
  fps: 0,
  wireFps: 0,
  latencyMs: null,
  bytesPerSec: 0,
  frames: 0,
  dropped: 0,
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

/// The core listens on an ephemeral port, so every launch is a new origin and
/// localStorage starts empty. Cookies ignore the port, so a small set of
/// preferences is mirrored there to survive a restart.
function readCookie(name: string): string {
  try {
    const prefix = `${name}=`
    for (const part of document.cookie.split(';')) {
      const entry = part.trim()
      if (entry.startsWith(prefix)) {
        return decodeURIComponent(entry.slice(prefix.length))
      }
    }
  } catch {
    // Cookies can be blocked; the in-memory default still works.
  }
  return ''
}

function writeCookie(name: string, value: string): void {
  try {
    document.cookie = `${name}=${encodeURIComponent(
      value,
    )}; path=/; max-age=31536000; SameSite=Lax`
  } catch {
    // Ignore blocked cookies.
  }
}

export function initialTheme(): Theme {
  const forced = new URLSearchParams(window.location.search).get('theme')
  if (forced === 'light' || forced === 'dark') return forced
  const saved = readStorage(THEME_KEY) || readCookie('molink_theme')
  if (saved === 'light' || saved === 'dark') return saved
  return window.matchMedia?.('(prefers-color-scheme: light)').matches
    ? 'light'
    : 'dark'
}

function initialQualityMode(): QualityMode {
  const saved = readStorage(QUALITY_MODE_KEY)
  if (
    saved === 'turbo' ||
    saved === 'smooth' ||
    saved === 'balanced' ||
    saved === 'quality' ||
    saved === 'ultra' ||
    saved === 'lossless' ||
    saved === 'game' ||
    saved === 'auto'
  ) {
    return saved
  }
  return 'auto'
}

function initialHistoryView(): HistoryView {
  return readStorage(HISTORY_VIEW_KEY) === 'list' ? 'list' : 'cards'
}

function initialSessionMode(): SessionMode {
  return readStorage(MODE_KEY) === 'view' ? 'view' : 'control'
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

function initialUiMode(): UiMode {
  const forced = new URLSearchParams(window.location.search).get('ui')
  if (forced === 'simple' || forced === 'advanced') return forced
  const saved = readCookie('molink_uiMode') || readStorage(UI_MODE_KEY)
  return saved === 'advanced' ? 'advanced' : 'simple'
}

function initialConnectDraft(): ConnectDraft {
  try {
    const parsed = JSON.parse(readStorage(CONNECT_DRAFT_KEY)) as ConnectDraft
    if (parsed && typeof parsed.input === 'string' &&
        typeof parsed.code === 'string') {
      return { input: parsed.input, code: parsed.code }
    }
  } catch {
    // Fall through to an empty draft.
  }
  return { input: '', code: '' }
}

function initialSavedDevices(): SavedDevice[] {
  try {
    const parsed = JSON.parse(readStorage(SAVED_DEVICES_KEY)) as SavedDevice[]
    if (!Array.isArray(parsed)) return []
    return parsed
      .filter(
        (item) =>
          typeof item?.host === 'string' &&
          typeof item?.port === 'number' &&
          typeof item?.label === 'string',
      )
      .slice(0, 50)
  } catch {
    return []
  }
}

const runtimes = new Map<string, SessionConnection>()
let nextId = 1

/// The active tab pulls at full rate (SessionView); the others keep a slow
/// pull so their session stays live and their stored frame stays current.
function setForegroundRuntime(activeId: string): void {
  for (const [id, connection] of runtimes) {
    connection.setForeground(id === activeId)
  }
}

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
    if (patch.phase !== undefined) get().syncUiState()
  }

  let persistTimer: number | null = null
  const schedulePersist = (): void => {
    if (persistTimer !== null) window.clearTimeout(persistTimer)
    persistTimer = window.setTimeout(() => {
      persistTimer = null
      const token = get().token
      if (!token) return
      const saved = get().savedDevices.map((device) => ({
        key: device.key,
        id: device.id,
        host: device.host,
        port: device.port,
        label: device.label,
        platform: device.platform,
        // Keep ui.json small: only preset avatars are persisted; uploaded
        // images are fetched from the directory when the device is online.
        avatar:
          device.avatar && !device.avatar.startsWith('data:')
            ? device.avatar
            : undefined,
        token: device.token,
        savedAt: device.savedAt,
      }))
      void saveUiPreferences(token, {
        uiMode: get().uiMode,
        savedDevices: saved,
        recents: get().recents,
        connectDraft: get().connectDraft,
      }).catch(() => undefined)
    }, 400)
  }

  const remember = (
    host: string,
    port: number,
    label: string,
    token: string,
    saveToken: boolean,
    meta?: { id?: string; platform?: string; avatar?: string },
  ): void => {
    const recent: RecentConnection = {
      host,
      port,
      label,
      at: Date.now(),
      ...(meta?.id ? { id: meta.id } : {}),
      ...(meta?.platform ? { platform: meta.platform } : {}),
      ...(meta?.avatar && !meta.avatar.startsWith('data:')
        ? { avatar: meta.avatar }
        : {}),
    }
    const previous = get().recents.find(
      (item) => item.host === host && item.port === port,
    )
    if (saveToken && token.trim() !== '') {
      recent.token = token.trim()
    } else if (!saveToken && previous?.token) {
      // A view-only or popout connection must not erase a saved code.
      recent.token = previous.token
    }
    const recents = [
      recent,
      ...get().recents.filter(
        (item) => !(item.host === host && item.port === port),
      ),
    ].slice(0, 8)
    writeStorage(RECENT_KEY, JSON.stringify(recents))
    set({ recents })
    schedulePersist()
  }

  return {
    theme: initialTheme(),
    qualityWatermark: readStorage(QUALITY_KEY) === '1',
    qualityMode: initialQualityMode(),
    settingsOpen: false,
    windowFullscreen: false,
    immersive: false,
    historyView: initialHistoryView(),
    preferredMode: initialSessionMode(),
    forceRelay: readStorage('molink.forceRelay') === '1',
    uiMode: initialUiMode(),
    savedDevices: initialSavedDevices(),
    authPhase: 'booting',
    localMode: false,
    token: '',
    locked: false,
    lockTimeout: 60,
    server: null,
    stateError: null,
    device: null,
    security: null,
    nearby: [],
    tabs: [],
    activeTabId: 'home',
    recents: initialRecents(),
    connectDraft: initialConnectDraft(),

    setTheme: (theme) => {
      writeStorage(THEME_KEY, theme)
      writeCookie('molink_theme', theme)
      set({ theme })
    },

    toggleTheme: () => {
      const theme = get().theme === 'dark' ? 'light' : 'dark'
      writeStorage(THEME_KEY, theme)
      writeCookie('molink_theme', theme)
      set({ theme })
    },

    setQualityWatermark: (enabled) => {
      writeStorage(QUALITY_KEY, enabled ? '1' : '0')
      set({ qualityWatermark: enabled })
    },

    setQualityMode: (mode) => {
      writeStorage(QUALITY_MODE_KEY, mode)
      set({ qualityMode: mode })
    },

    setSettingsOpen: (open) => set({ settingsOpen: open }),

    setWindowFullscreen: (value) => {
      if (shellInfo.enabled) {
        setNativeFullscreen(value)
      } else if (value) {
        void document.documentElement.requestFullscreen?.().catch(() => undefined)
      } else if (document.fullscreenElement) {
        void document.exitFullscreen().catch(() => undefined)
      }
      set({ windowFullscreen: value })
    },

    setImmersive: (value) => set({ immersive: value }),

    setHistoryView: (view) => {
      writeStorage(HISTORY_VIEW_KEY, view)
      set({ historyView: view })
    },

    setForceRelay: (value) => {
      writeStorage('molink.forceRelay', value ? '1' : '0')
      set({ forceRelay: value })
    },
    setPreferredMode: (mode) => {
      writeStorage(MODE_KEY, mode)
      set({ preferredMode: mode })
    },

    setUiMode: (mode) => {
      writeStorage(UI_MODE_KEY, mode)
      writeCookie('molink_uiMode', mode)
      set({ uiMode: mode })
      schedulePersist()
    },

    setConnectDraft: (patch) => {
      const next: ConnectDraft = { ...get().connectDraft, ...patch }
      writeStorage(CONNECT_DRAFT_KEY, JSON.stringify(next))
      set({ connectDraft: next })
      schedulePersist()
    },

    loadUiPreferences: async () => {
      const token = get().token
      if (!token) return
      try {
        const prefs = await fetchUiPreferences(token)
        const next: Partial<AppState> = {}
        if (prefs.uiMode === 'simple' || prefs.uiMode === 'advanced') {
          next.uiMode = prefs.uiMode
          writeStorage(UI_MODE_KEY, prefs.uiMode)
          writeCookie('molink_uiMode', prefs.uiMode)
        }
        if (
          Array.isArray(prefs.savedDevices) &&
          (prefs.savedDevices.length > 0 || get().savedDevices.length === 0)
        ) {
          next.savedDevices = prefs.savedDevices.slice(0, 50).map((device) => ({
            key:
              device.key ||
              (device.id
                ? `id:${device.id}`
                : `host:${device.host}:${device.port}`),
            id: device.id,
            host: device.host,
            port: device.port,
            label: device.label,
            platform: device.platform,
            avatar: device.avatar,
            token: device.token,
            savedAt: device.savedAt || Date.now(),
          }))
          writeStorage(
            SAVED_DEVICES_KEY,
            JSON.stringify(next.savedDevices),
          )
        }
        if (Array.isArray(prefs.recents) && prefs.recents.length > 0) {
          next.recents = prefs.recents.slice(0, 8)
          writeStorage(RECENT_KEY, JSON.stringify(next.recents))
        }
        const draft = prefs.connectDraft
        if (draft && typeof draft.input === 'string' &&
            typeof draft.code === 'string' &&
            (draft.input !== '' || draft.code !== '')) {
          next.connectDraft = { input: draft.input, code: draft.code }
          writeStorage(
            CONNECT_DRAFT_KEY,
            JSON.stringify(next.connectDraft),
          )
        }
        set(next)
      } catch {
        // Keep the local copy when the core is unavailable.
      }
    },

    saveDevice: (input) => {
      const id = input.id?.trim()
      const key =
        input.key ?? (id ? `id:${id}` : `host:${input.host}:${input.port}`)
      const existing = get().savedDevices.find((device) => device.key === key)
      const saved: SavedDevice = {
        ...existing,
        ...input,
        key,
        savedAt: existing?.savedAt ?? Date.now(),
      }
      const list = [
        saved,
        ...get().savedDevices.filter((device) => device.key !== key),
      ].slice(0, 50)
      writeStorage(SAVED_DEVICES_KEY, JSON.stringify(list))
      set({ savedDevices: list })
      schedulePersist()
    },

    patchSavedDevice: (id, patch) => {
      const list = get().savedDevices.map((device) =>
        device.id === id || device.key === id ? { ...device, ...patch } : device,
      )
      if (JSON.stringify(list) === JSON.stringify(get().savedDevices)) return
      writeStorage(SAVED_DEVICES_KEY, JSON.stringify(list))
      set({ savedDevices: list })
      schedulePersist()
    },

    removeSavedDevice: (key) => {
      const list = get().savedDevices.filter((device) => device.key !== key)
      writeStorage(SAVED_DEVICES_KEY, JSON.stringify(list))
      set({ savedDevices: list })
      schedulePersist()
    },

    setServerEnabled: async (enabled) => {
      const token = get().token
      if (!token) return false
      try {
        await postServerEnabled(token, enabled)
        await get().refreshState()
        return true
      } catch {
        set({ stateError: 'server_settings_failed' })
        return false
      }
    },

    setAvatarValue: async (avatar) => {
      const token = get().token
      if (!token) return false
      try {
        const result = await saveAvatar(token, avatar)
        const device = get().device
        if (device) set({ device: { ...device, avatar: result.avatar } })
        return true
      } catch {
        return false
      }
    },

    setDomain: async (action, code) => {
      const token = get().token
      if (!token) return 'failed'
      try {
        await postDomain(token, action, code)
        await get().refreshState()
        return 'ok'
      } catch (caught) {
        if (caught instanceof ApiError && caught.code === 'unknown_domain') {
          return 'unknown_domain'
        }
        return 'failed'
      }
    },

    refreshDomainDevices: async () => {
      const token = get().token
      if (!token || !get().localMode) return []
      try {
        const result = await fetchDomainDevices(token)
        return result.devices
      } catch {
        return []
      }
    },

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
            void get().refreshSecurity()
            void get().loadUiPreferences()
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
            void get().refreshSecurity()
            void get().loadUiPreferences()
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
          const info = await fetchInfo(result.session_token)
          capabilities = info.capabilities
        } catch {
          // capabilities are cosmetic; login already succeeded
        }
        const state = get().server
        const host =
          state?.host && state.host !== '0.0.0.0'
            ? state.host
            : window.location.hostname
        const port = state?.port || Number(window.location.port) || 28418
        set({
          token: result.session_token,
          locked: false,
          localMode: result.local,
          lockTimeout: result.lock_timeout,
          authPhase: 'ready',
          stateError: null,
          device: {
            device_id: result.device_id,
            token: result.session_token,
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
        if (result.local) {
          // Refresh the local device card (temporary code state) and use the
          // stable local session token rather than the login-issued one.
          try {
            const device = await fetchDeviceInfo()
            set({ device, token: device.token })
          } catch {
            // keep the login result
          }
          void get().refreshSecurity()
          void get().loadUiPreferences()
        }
        return true
      } catch (error) {
        let code = 'core_unreachable'
        if (error instanceof ApiError) {
          if (error.status === 401) code = 'invalid_token'
          else if (error.status === 423) code = 'locked'
          else if (error.status === 429) code = 'too_many_attempts'
          else if (error.status === 403) code = 'ip_banned'
        }
        set({ stateError: code })
        return false
      }
    },

    setNetworkPorts: async (tcp, udp) => {
      const token = get().token
      if (!token) return false
      try {
        const result = await postNetworkPorts(token, tcp, udp)
        const server = get().server
        set({
          server: server
            ? {
                ...server,
                tcp_port: result.tcp_port,
                udp_port: result.udp_port,
                restart_required: result.restart_required,
              }
            : server,
          stateError: null,
        })
        return true
      } catch {
        set({ stateError: 'settings_failed' })
        return false
      }
    },

    setAuthCode: async (mode, uses, value = '') => {
      const token = get().token
      if (!token) return null
      try {
        const code = await postAuthCode(token, mode, uses, value)
        const device = get().device
        if (device) set({ device: { ...device, code } })
        set({ stateError: null })
        return code
      } catch {
        set({ stateError: 'settings_failed' })
        return null
      }
    },

    refreshSecurity: async () => {
      const token = get().token
      if (!token) return
      try {
        const security = await fetchSettings(token)
        set({ security })
      } catch {
        // Remote sessions receive 403 here; keep the previous value.
      }
    },

    refreshDiscovery: async () => {
      const token = get().token
      if (!token) return
      try {
        const result = await fetchDiscovery(token)
        set({ nearby: result.devices })
      } catch {
        // Discovery is local-only; remote sessions simply keep an empty list.
      }
    },

    refreshDevice: async () => {
      if (!get().localMode) return
      const token = get().token
      if (!token) return
      try {
        const device = await fetchDeviceInfo()
        set({ device, token: device.token })
      } catch {
        // Keep the last known code on transient failures.
      }
    },

    saveSecurity: async (values) => {
      const token = get().token
      if (!token) return false
      try {
        const security = await postSettings(token, values)
        set({ security })
        return true
      } catch {
        set({ stateError: 'settings_failed' })
        return false
      }
    },

    decideAccessRequest: async (id, allow) => {
      const token = get().token
      if (!token) return
      try {
        await postAccessDecision(token, id, allow)
      } catch {
        // The request may already have been answered or expired.
      }
      await get().refreshState()
    },

    setLongTermCode: async (value) => {
      const token = get().token
      if (!token) return false
      try {
        const longTermCode = await postLongTermCode(token, value)
        const security = get().security
        if (security) set({ security: { ...security, long_term_code: longTermCode } })
        return true
      } catch {
        set({ stateError: 'settings_failed' })
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
      const active = get().tabs.filter(
        (tab) => tab.phase === 'live' || tab.phase === 'connecting',
      ).length
      void postUiState(token, UI_CLIENT_ID, active).catch(() => undefined)
    },

    rememberRecent: (host, port, label, token = '', saveToken = false) => {
      remember(host, port, label, token, saveToken)
    },

    createSession: (host, port, token, label, saveToken = false,
                    requestAccess = false,
                    mode = get().preferredMode,
                    intent: SessionIntent = 'desktop',
                    deviceId,
                    platform,
                    avatar) => {
      const id = `session-${nextId++}`
      const trimmedToken = token.trim()
      const tab: SessionTab = {
        id,
        label: label?.trim() || host,
        host: host.trim(),
        port,
        token: trimmedToken,
        deviceId,
        phase: 'connecting',
        welcome: null,
        frame: null,
        cursorShape: null,
        cursorPosition: null,
        stats: EMPTY_STATS,
        error: null,
        notice: null,
        approvalDeadline: null,
        retryAt: null,
        controlEnabled: mode === 'control' && intent === 'desktop',
        mode,
        intent,
        clipboardEnabled: true,
        displayIndex: 0,
      }
      set((state) => ({
        tabs: [...state.tabs, tab],
        activeTabId: id,
      }))
      get().syncUiState()

      const connection = new SessionConnection({
        onPatch: (patch) => {
          patchTab(id, patch)
          // Once the peer reports its computer name, use it in history too
          // instead of the address that was typed before connecting.
          if (patch.welcome?.device_name) {
            get().relabelRecent(tab.host, port, patch.welcome.device_name)
          }
        },
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
      setForegroundRuntime(id)
      if (get().localMode) {
        // Local files go to every remote; the local core's clipboard API is
        // only reachable when this UI is the local shell/browser.
        connection.enableClipboardFileSync(() => get().token)
      }

      if (trimmedToken === '' && !requestAccess) {
        patchTab(id, { phase: 'error', error: 'no_token' })
      } else {
        // A shell knows its own machine; a remote browser only sees the
        // host's identity, so it stays anonymous and the host labels it by
        // peer address.
        const self = get()
        const clientName =
          self.localMode ? self.device?.device_name ?? '' : ''
        const clientId =
          self.localMode ? self.device?.device_id ?? '' : ''
        // Windows 7's UDP stack and fragmentation path are not reliable for
        // this app; keep that client on TCP (relay or direct WebSocket) and
        // skip ICE entirely. The relay's file_data path is TCP too.
        const windows7 =
          typeof navigator !== 'undefined' &&
          /Windows NT 6\.1/i.test(navigator.userAgent)
        connection.connect(tab.host, port, trimmedToken, requestAccess,
                           deviceId, self.forceRelay || windows7,
                           clientName, clientId)
        if (trimmedToken !== '') {
          remember(tab.host, port, tab.label, trimmedToken, saveToken, {
            id: deviceId,
            platform,
            avatar,
          })
        }
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
      setForegroundRuntime(get().activeTabId)
      get().syncUiState()
    },

    setActiveTab: (id) => {
      set({ activeTabId: id })
      setForegroundRuntime(id)
    },

    setControlEnabled: (id, enabled) =>
      patchTab(id, { controlEnabled: enabled }),

    setClipboardEnabled: (id, enabled) => {
      patchTab(id, { clipboardEnabled: enabled })
      runtimes.get(id)?.setClipboardEnabled(enabled)
    },

    forgetRecent: (at) => {
      // The list is persisted below via schedulePersist.
      const recents = get().recents.filter((item) => item.at !== at)
      writeStorage(RECENT_KEY, JSON.stringify(recents))
      set({ recents })
      schedulePersist()
    },

    relabelRecent: (host, port, label) => {
      let changed = false
      const recents = get().recents.map((item) => {
        if (item.host === host && item.port === port && item.label !== label) {
          changed = true
          return { ...item, label }
        }
        return item
      })
      if (!changed) return
      writeStorage(RECENT_KEY, JSON.stringify(recents))
      set({ recents })
      schedulePersist()
    },
  }
})
