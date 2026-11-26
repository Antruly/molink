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
  postCodePolicy,
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
  type UiPreferences,
} from '../lib/api'
import type {
  ClientPhase,
  CursorPosition,
  CursorShape,
  Welcome,
} from '../lib/client'
import { cleanDeviceNote, deviceNoteKey } from '../lib/device-note'
import {
  normalizeComboForwardAsked,
  type ComboForwardChoice,
} from '../lib/combo-forward'
import { logDiag } from '../lib/diag-log'
import type { ClipboardOfferPrompt } from '../lib/remote-clipboard'
import type { Frame } from '../lib/protocol'
import { forgetSessionScreenBuffer } from '../lib/screen-buffer'
import {
  ADVANCED_QUALITY_KEY,
  ADVANCED_STORAGE_PREFIX,
  DEFAULT_SESSION_PRESET,
  PRESET_STORAGE_PREFIX,
  advancedStorageKey,
  defaultAdvancedParams,
  normalizeAdvancedParams,
  normalizePresetId,
  parseStoredAdvanced,
  parseStoredPreset,
  presetStorageKey,
  readStoredAdvanced,
  writeStoredAdvanced,
  writeStoredPreset,
  type AdvancedQualityParams,
  type SessionPresetId,
  type StoredSessionPreset,
  type PresetApplied,
} from '../lib/session-presets'
import {
  SessionConnection,
  type Stats,
  type TabPatch,
} from '../lib/session-connection'
import {
  TOAST_TTL_MS,
  applyFailure,
  toastExpired,
  toastFor,
  type SessionToast,
  type ToastTone,
} from '../lib/session-alerts'
import {
  setWindowFullscreen as setNativeFullscreen,
  shellInfo,
} from '../lib/shell'
import {
  kRememberDeviceKey,
  sessionStore,
} from '../lib/session-store'

export type Theme = 'light' | 'dark'
export type AuthPhase = 'booting' | 'login' | 'locked' | 'ready'
/// Picture presets are viewer-owned and stored per host:port. "auto" is one
/// of the presets; only it may adapt. See lib/session-presets.ts.
export type { SessionPresetId } from '../lib/session-presets'
/// A control session starts with input enabled; a view-only session starts
/// with it off. The session toolbar can still change it later.
export type SessionMode = 'control' | 'view'
import {
  nextVisibleTabId,
  type PaneKind,
  type SessionIntent,
} from '../lib/session-intent'
export type { SessionIntent } from '../lib/session-intent'
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
  /// A remote clipboard selection waiting behind the explicit "取出" button,
  /// or its download progress.
  clipboardOffer?: ClipboardOfferPrompt | null
  /// Transient corner message for a feature-level failure (clipboard, input,
  /// one file action). Cleared automatically; never blocks the session.
  toast?: SessionToast | null
  /// "relay" or "p2p" for a device-ID session (legacy live mode).
  transport?: string
  /// Data path reported live by the local core: direct | p2p | relay.
  path?: string
  /// Wire protocol reported live: websocket | quic | udp | tcp.
  protocol?: string
  /// The client core is decoding AV1 patches for this session.
  av1Active?: boolean
  /// Host is streaming H.264 for this tab (the low-delay stream).
  streamCodec?: string
  /// Stream codecs the session toolbar may offer (host can encode + page can
  /// decode), and the host's current global preference.
  codecOptions?: string[]
  codecPreference?: string
  /// False when the host's pinned encoder cannot run for this session; the
  /// toolbar names `codecError` instead of silently substituting a codec.
  codecAvailable?: boolean
  codecError?: string
  /// The stream is AV1-only (no tile image underneath).
  av1Full?: boolean
  av1WireBytes?: number
  av1DecodedBytes?: number
  /// Host-reported capture rate and encoder build time (EMA), refreshed once
  /// per second. The frame-rate controller uses the build time to notice a
  /// slow encoder.
  hostFps?: number
  hostBuildMs?: number
  /// Viewer-owned picture preset for this host:port. Never written back to
  /// the host's settings.
  preset: SessionPresetId
  /// The advanced panel's per-device parameters. Carried even while advanced
  /// mode is off, so reopening it restores the last custom picture.
  advanced?: AdvancedQualityParams
  /// What the host reported it actually applied (fps/codec/chroma).
  appliedPreset?: PresetApplied
  notice: string | null
  /// When the host must approve this session: epoch ms of the 60 s deadline.
  approvalDeadline: number | null
  /// Epoch ms of the next automatic reconnect attempt after a disconnect.
  retryAt: number | null
  controlEnabled: boolean
  mode: SessionMode
  /// What the simple home asked for before the session started.
  intent: SessionIntent
  /// Folded to a minimal state in the tab bar; the session stays alive.
  minimized?: boolean
  /// A pane tab (files/chat/terminal promoted out of a desktop drawer) reuses
  /// the owner's connection instead of opening its own. This is the tab id
  /// that owns the SessionConnection.
  runtimeTabId?: string
  /// The app-level pane this tab displays. Panes are mounted once in
  /// PaneHost so moving one from a drawer to a tab keeps its state.
  paneId?: string
  clipboardEnabled: boolean
  displayIndex: number
}

/// One chat/files/terminal panel. It lives in PaneHost for as long as it is
/// open, so promoting it from a desktop drawer to its own tab does not rebuild
/// its WebSocket, terminal buffer or scroll position.
export interface SessionPane {
  id: string
  kind: PaneKind
  /// Tab whose SessionConnection serves this pane.
  ownerTabId: string
  /// Set when the pane has its own tab; the slot lives in that page.
  standaloneTabId?: string
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
  /// Fade a session overlay to near-transparent while the pointer is within
  /// its proximity radius. On by default; touch input never fades.
  watermarkFade: boolean
  /// Play the remote machine's sound as soon as a session is live.
  autoAudio: boolean
  /// Remote Explorer file clipboard -> this machine's clipboard. Off by
  /// default: enabling it transfers the whole file selection over the session.
  remoteClipboardFiles: boolean
  /// Mount remote clipboard files with FUSE (Linux) and pull ranges only when
  /// this machine pastes. On by default; when the core cannot mount, the
  /// selection falls back to the capped download path.
  clipboardVirtualFiles: boolean
  /// Keep the host's reconnect credential across restarts, so a single-use
  /// verification code is not needed again. On by default.
  rememberDevice: boolean
  /// X11-only: while the remote picture is focused, grab the keyboard so the
  /// desktop's Super/Alt shortcuts reach the remote. Off by default because it
  /// also takes Alt+Tab and friends away from the local desktop.
  keyboardGrab: boolean
  /// Type through the local IME and send the committed text as `{k:"text"}`.
  /// On by default; off makes every device, desktop included, forward key
  /// positions only (the touch virtual keyboard keeps using text).
  localIme: boolean
  /// The remembered answer to the first-use "enable combo forwarding?"
  /// dialog. null = never answered with "remember"; the dialog asks again on
  /// a later launch.
  comboForwardAsked: ComboForwardChoice | null
  /// In-memory only: the dialog was answered this page session. An
  /// unremembered answer silences it until the next launch.
  comboForwardAnswered: boolean
  /// What the window's close button does while a session tab is open:
  /// ask once, hide to the tray, or close the session tab.
  windowCloseBehavior: 'ask' | 'tray' | 'tab'
  /// Default picture preset for a new session. Only affects this machine when
  /// it is the viewer/controller; a host:port with a remembered choice wins.
  defaultPreset: SessionPresetId
  /// Viewer-owned presets keyed by `molink.preset.<host>:<port>`, mirrored to
  /// ui.json so the native shell survives its changing origin.
  presets: Record<string, StoredSessionPreset>
  /// Advanced画质 mode: the toolbar replaces the six preset buttons with the
  /// per-device fps/quality/bitrate/chroma/codec panel. Global to this viewer.
  advancedQuality: boolean
  /// Advanced parameters keyed by `molink.advanced.<host>:<port>`.
  advanced: Record<string, AdvancedQualityParams>
  /// A toolbar popover (the quality panel) is open. The keyboard stays local
  /// while it is, the same rule as the settings dialog.
  qualityPanelOpen: boolean
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
  /// Six-digit TOTP for this connection attempt. Set by the connect form and
  /// consumed by the next hello; a host without two-factor ignores it.
  twoFactorCode: string
  uiMode: UiMode
  savedDevices: SavedDevice[]
  /// User notes keyed by the normalized device name. Saved devices, recents,
  /// LAN peers and the online list all read the same map, so giving a machine
  /// a note updates every card that shows it.
  deviceNotes: Record<string, string>
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
  /// Chat/files/terminal panels mounted once at the app root. They outlive a
  /// tab switch and can move from a desktop drawer to their own tab without
  /// being rebuilt.
  panes: SessionPane[]
  activeTabId: string
  recents: RecentConnection[]
  connectDraft: ConnectDraft

  setTheme: (theme: Theme) => void
  toggleTheme: () => void
  setQualityWatermark: (enabled: boolean) => void
  setWatermarkFade: (enabled: boolean) => void
  setAutoAudio: (enabled: boolean) => void
  setRemoteClipboardFiles: (enabled: boolean) => void
  setClipboardVirtualFiles: (enabled: boolean) => void
  /// Banner state for a remote clipboard selection that needs an explicit
  /// "取出" (or is downloading it).
  setClipboardOffer: (
    id: string,
    prompt: ClipboardOfferPrompt | null,
  ) => void
  /// Replaces the tab's toast directly (null dismisses it).
  setTabToast: (id: string, toast: SessionToast | null) => void
  /// Raises a transient message; a repeat of the same text refreshes it.
  pushToast: (id: string, message: string, tone?: ToastTone) => void
  setRememberDevice: (enabled: boolean) => void
  setKeyboardGrab: (enabled: boolean) => void
  setLocalIme: (enabled: boolean) => void
  /// Stores the remembered first-use answer (`null` clears it).
  setComboForwardAsked: (choice: ComboForwardChoice | null) => void
  /// Marks the dialog answered for this page session; never persisted.
  markComboForwardAnswered: () => void
  setWindowCloseBehavior: (behavior: 'ask' | 'tray' | 'tab') => void
  /// Sets the default preset for future sessions (viewer-side only).
  setDefaultPreset: (preset: SessionPresetId) => void
  /// Changes one tab's preset and remembers it for that host:port.
  setTabPreset: (id: string, preset: SessionPresetId) => void
  /// Turns advanced mode on/off. "回到预设" calls this with false; the tab's
  /// last preset tier is still selected underneath.
  setAdvancedQuality: (enabled: boolean) => void
  /// Merges one advanced parameter into a tab and remembers it per device.
  setTabAdvanced: (id: string, patch: Partial<AdvancedQualityParams>) => void
  setQualityPanelOpen: (open: boolean) => void
  setSettingsOpen: (open: boolean) => void
  setWindowFullscreen: (value: boolean) => void
  setImmersive: (value: boolean) => void
  setHistoryView: (view: HistoryView) => void
  setPreferredMode: (mode: SessionMode) => void
  setForceRelay: (value: boolean) => void
  setTwoFactorCode: (value: string) => void
  setUiMode: (mode: UiMode) => void
  setConnectDraft: (patch: Partial<ConnectDraft>) => void
  loadUiPreferences: () => Promise<void>
  saveDevice: (device: Omit<SavedDevice, 'key' | 'savedAt'> & { key?: string }) => void
  patchSavedDevice: (id: string, patch: Partial<SavedDevice>) => void
  removeSavedDevice: (key: string) => void
  /// Empty note removes the entry for that device name.
  setDeviceNote: (name: string, note: string) => void
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
  setCodePolicy: (
    single: boolean,
    multi: boolean,
  ) => Promise<AuthCode | null>
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
    syncBetweenSessions?: boolean
    encryptRemote?: boolean
    encryptLan?: boolean
    name: string
    stunServer?: string
    ipAllowlist?: string
    portForwardEnabled?: boolean
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
  /// Fold a tab to its minimal state in the tab bar without closing it. The
  /// session and every pane it owns stay alive.
  setTabMinimized: (id: string, minimized: boolean) => void
  /// Opens (or re-focuses) one panel drawer for a desktop session. Other
  /// panes of the same session close, matching the old exclusive drawer.
  openPane: (ownerTabId: string, kind: PaneKind) => string
  /// Ends one pane: a terminal WebSocket closes, a file panel disconnects.
  closePane: (id: string) => void
  /// Moves an embedded pane into its own tab, reusing the owner's connection
  /// and the already mounted panel (no terminal/process rebuild).
  promotePane: (id: string) => string | null
  setControlEnabled: (id: string, enabled: boolean) => void
  setClipboardEnabled: (id: string, enabled: boolean) => void
  forgetRecent: (at: number) => void
  relabelRecent: (host: string, port: number, label: string) => void
}

const THEME_KEY = 'molink.theme'
const QUALITY_KEY = 'molink.quality'
const WATERMARK_FADE_KEY = 'molink.watermarkFade'
const AUTO_AUDIO_KEY = 'molink.autoAudio'
const REMOTE_CLIPBOARD_FILES_KEY = 'molink.remoteClipboardFiles'
const CLIPBOARD_VIRTUAL_FILES_KEY = 'molink.clipboardVirtualFiles'
const CLOSE_BEHAVIOR_KEY = 'molink.closeBehavior'
const QUALITY_MODE_KEY = 'molink.qualityMode'
const DEFAULT_PRESET_KEY = 'molink.defaultPreset'
const RECENT_KEY = 'molink.recents'
const HISTORY_VIEW_KEY = 'molink.historyView'
const MODE_KEY = 'molink.sessionMode'
const UI_MODE_KEY = 'molink.uiMode'
const SAVED_DEVICES_KEY = 'molink.savedDevices'
const DEVICE_NOTES_KEY = 'molink.deviceNotes'
const CONNECT_DRAFT_KEY = 'molink.connectDraft'
/// Read by session-store too; kept in one place so the module and the UI can
/// never disagree about the default.
const REMEMBER_DEVICE_KEY = kRememberDeviceKey
const KEYBOARD_GRAB_KEY = 'molink.keyboardGrab'
const LOCAL_IME_KEY = 'molink.localIme'
const COMBO_FORWARD_KEY = 'molink.comboForwardAsked'
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

/// The default for a session with no remembered per-host choice. The legacy
/// `molink.qualityMode` is read once so an existing install keeps the tier it
/// already had instead of jumping to the new default.
function initialDefaultPreset(): SessionPresetId {
  const saved =
    readStorage(DEFAULT_PRESET_KEY) || readStorage(QUALITY_MODE_KEY)
  if (saved === 'turbo') return 'turbo'
  if (saved === 'smooth') return 'smooth'
  if (saved === 'balanced') return 'balanced'
  if (saved === 'quality' || saved === 'ultra' || saved === 'lossless') {
    return 'hd'
  }
  if (saved === 'auto') return 'auto'
  return DEFAULT_SESSION_PRESET
}

/// localStorage remembers per host:port. The map is also copied to ui.json by
/// the store snapshot, so a native shell restart (new ephemeral origin) keeps
/// every device's choice.
function initialPresets(): Record<string, StoredSessionPreset> {
  const presets: Record<string, StoredSessionPreset> = {}
  try {
    const storage = window.localStorage
    for (let index = 0; index < storage.length; index += 1) {
      const key = storage.key(index)
      if (!key || !key.startsWith(PRESET_STORAGE_PREFIX)) continue
      const parsed = parseStoredPreset(storage.getItem(key))
      if (parsed) presets[key] = parsed
    }
  } catch {
    // Blocked storage just means no remembered presets.
  }
  return presets
}

/// Same mirror as initialPresets(): localStorage is the fast copy, ui.json is
/// what survives the native shell's changing origin.
function initialAdvanced(): Record<string, AdvancedQualityParams> {
  const advanced: Record<string, AdvancedQualityParams> = {}
  try {
    const storage = window.localStorage
    for (let index = 0; index < storage.length; index += 1) {
      const key = storage.key(index)
      if (!key || !key.startsWith(ADVANCED_STORAGE_PREFIX)) continue
      const parsed = parseStoredAdvanced(storage.getItem(key))
      if (parsed) advanced[key] = parsed
    }
  } catch {
    // Blocked storage just means no remembered advanced parameters.
  }
  return advanced
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

function sanitizeDeviceNotes(
  value: unknown,
): Record<string, string> {
  const notes: Record<string, string> = {}
  if (value === null || typeof value !== 'object') return notes
  for (const [name, note] of Object.entries(
    value as Record<string, unknown>,
  )) {
    if (typeof note !== 'string') continue
    const key = deviceNoteKey(name)
    const clean = cleanDeviceNote(note)
    if (key && clean) notes[key] = clean
  }
  return notes
}

function initialDeviceNotes(): Record<string, string> {
  try {
    return sanitizeDeviceNotes(JSON.parse(readStorage(DEVICE_NOTES_KEY)))
  } catch {
    return {}
  }
}

const runtimes = new Map<string, SessionConnection>()
let nextId = 1
let nextPaneId = 1

/// Resolve a tab id to the connection that actually serves it. A promoted
/// pane tab shares its owner's SessionConnection, so closing the drawer and
/// opening the tab does not reconnect (or restart a terminal).
function runtimeIdFor(tabId: string): string {
  const tab = useAppStore.getState().tabs.find((item) => item.id === tabId)
  return tab?.runtimeTabId ?? tabId
}

/// The active tab pulls at full rate (SessionView); the others keep a slow
/// pull so their session stays live and their stored frame stays current.
function setForegroundRuntime(activeId: string): void {
  // `activeId` may be a promoted pane tab; its connection is registered under
  // the owner's runtime id.
  const runtimeId = runtimeIdFor(activeId)
  for (const [id, connection] of runtimes) {
    connection.setForeground(id === runtimeId)
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
  // `id` is a tab id; a promoted pane tab points at its owner's runtime.
  const resolved = runtimeIdFor(id)
  return runtimes.get(id) ?? runtimes.get(resolved)
}

export const useAppStore = create<AppState>((set, get) => {
  const patchTab = (id: string, patch: TabPatch): void => {
    set((state) => ({
      tabs: state.tabs.map((tab) =>
        // A promoted pane tab shares the owner's connection; the welcome,
        // phase and errors travel to both so the pane page and the desktop
        // tab never disagree.
        tab.id === id || tab.runtimeTabId === id
          ? { ...tab, ...patch }
          : tab,
      ),
    }))
    if (patch.phase !== undefined) get().syncUiState()
  }

  // One toast per tab. `nextToastId` makes clear timers safe: a timer armed
  // for an old message cannot dismiss a newer one.
  let nextToastId = 1
  const toastTimers = new Map<string, number>()

  const clearToastTimer = (tabId: string): void => {
    const timer = toastTimers.get(tabId)
    if (timer === undefined) return
    window.clearTimeout(timer)
    toastTimers.delete(tabId)
  }

  const scheduleToastClear = (tabId: string, toastId: number): void => {
    clearToastTimer(tabId)
    const timer = window.setTimeout(() => {
      const current = get().tabs.find((tab) => tab.id === tabId)?.toast
      if (current === null || current === undefined || current.id !== toastId) {
        // Cleared or replaced by a newer toast, whose own timer is armed.
        toastTimers.delete(tabId)
        return
      }
      if (!toastExpired(current, Date.now())) {
        // Woke early (a clock step): re-arm instead of leaving it up forever.
        scheduleToastClear(tabId, toastId)
        return
      }
      toastTimers.delete(tabId)
      patchTab(tabId, { toast: null })
    }, TOAST_TTL_MS)
    toastTimers.set(tabId, timer)
  }

  /// Raises a transient corner message. The same text refreshes the existing
  /// toast instead of stacking a second box.
  const pushToast = (
    id: string,
    message: string,
    tone: ToastTone = 'error',
  ): void => {
    const tab = get().tabs.find((item) => item.id === id)
    if (!tab) return
    const toast = toastFor(tab.toast, message, tone, nextToastId++, Date.now())
    patchTab(id, { toast })
    scheduleToastClear(id, toast.id)
  }

  /// Single entry point for failures reported by a connection. Feature-level
  /// failures (clipboard, one input event, a file action) become a toast and
  /// leave the session banner alone; only session-level failures reach
  /// `tab.error`, and only some of those offer a reconnect.
  const reportFailure = (
    id: string,
    message: string,
    code: string | null = null,
    tone?: ToastTone,
  ): void => {
    const tab = get().tabs.find((item) => item.id === id)
    if (!tab) return
    const result = applyFailure(
      {
        error: tab.error,
        errorCode: tab.errorCode ?? null,
        toast: tab.toast ?? null,
      },
      message,
      code,
      nextToastId++,
      Date.now(),
      tone,
    )
    patchTab(id, {
      error: result.error,
      errorCode: result.errorCode,
      toast: result.toast,
      // A session-level failure supersedes the approval countdown.
      ...(result.surface === 'banner' ? { notice: null } : {}),
    })
    if (result.surface === 'toast' && result.toast !== null) {
      scheduleToastClear(id, result.toast.id)
    }
  }

  let persistTimer: number | null = null

  /// The complete ui.json document. Every save rewrites it, so the session
  /// credentials must be part of the same snapshot; otherwise a preference
  /// change would silently delete them.
  const uiPreferencesSnapshot = (): UiPreferences => ({
    uiMode: get().uiMode,
    language: (() => {
      try {
        return window.localStorage.getItem('molink.lang') || 'zh-CN'
      } catch {
        return 'zh-CN'
      }
    })(),
    savedDevices: get().savedDevices.map((device) => ({
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
    })),
    recents: get().recents,
    deviceNotes: get().deviceNotes,
    connectDraft: get().connectDraft,
    rememberDevice: get().rememberDevice,
    comboForwardAsked: get().comboForwardAsked ?? undefined,
    localIme: get().localIme,
    defaultPreset: get().defaultPreset,
    presets: get().presets,
    advancedQuality: get().advancedQuality,
    advanced: get().advanced,
    sessions: sessionStore.persistedRecords(),
  })

  /// Writes ui.json now. A session token arrives with `welcome`, and the user
  /// may kill the process a moment later; the usual 400 ms debounce is too
  /// long to promise a restart can reconnect.
  const persistUiPreferencesNow = (): void => {
    const token = get().token
    // The endpoint is loopback-only; a remote browser's copy lives in
    // localStorage instead.
    if (!token || !get().localMode) return
    void saveUiPreferences(token, uiPreferencesSnapshot()).catch(
      () => undefined,
    )
  }

  /// One persistent backend at a time. The native shell (and a browser served
  /// by the local core) uses ui.json; a remote browser falls back to
  /// localStorage. Never both, so the two copies cannot drift.
  const configureSessionBackend = (local: boolean): void => {
    if (!local) {
      sessionStore.useLocalStorage()
      return
    }
    // The ui.json endpoint needs the local token; switching before it exists
    // would clear localStorage without being able to write the file store.
    if (!get().token) return
    sessionStore.useExternalPersistence(persistUiPreferencesNow)
  }

  const schedulePersist = (): void => {
    if (persistTimer !== null) window.clearTimeout(persistTimer)
    persistTimer = window.setTimeout(() => {
      persistTimer = null
      persistUiPreferencesNow()
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
    // One row per device: a session opened by device ID and later by address
    // is the same machine, and the list used to show it two or three times
    // (a, b, a). The newest entry wins and replaces the older ones.
    const sameDevice = (item: RecentConnection) =>
      (meta?.id !== undefined && item.id === meta.id) ||
      (item.host === host && item.port === port)
    const previous = get().recents.find(sameDevice)
    if (saveToken && token.trim() !== '') {
      recent.token = token.trim()
    } else if (!saveToken && previous?.token) {
      // A view-only or popout connection must not erase a saved code.
      recent.token = previous.token
    }
    const recents = [
      recent,
      ...get().recents.filter((item) => !sameDevice(item)),
    ].slice(0, 8)
    writeStorage(RECENT_KEY, JSON.stringify(recents))
    set({ recents })
    schedulePersist()
  }

  return {
    theme: initialTheme(),
    qualityWatermark: readStorage(QUALITY_KEY) === '1',
    // On by default: the overlays are useful when looked at, not while the
    // pointer is crossing the picture to reach them.
    watermarkFade: readStorage(WATERMARK_FADE_KEY) !== '0',
    autoAudio: readStorage(AUTO_AUDIO_KEY) !== '0',
    // On by default: pasting a file copied on the remote is the feature, and
    // the settings switch still lets a user opt out.
    remoteClipboardFiles: readStorage(REMOTE_CLIPBOARD_FILES_KEY) !== '0',
    // On by default: virtual paste is the reason copying a remote file no
    // longer downloads it. The switch opts back into eager downloading.
    clipboardVirtualFiles:
      readStorage(CLIPBOARD_VIRTUAL_FILES_KEY) !== '0',
    rememberDevice: readStorage(REMEMBER_DEVICE_KEY) !== '0',
    // The shell's origin changes with its ephemeral port, so the cookie copy
    // is what actually survives a restart.
    keyboardGrab:
      readStorage(KEYBOARD_GRAB_KEY) === '1' ||
      readCookie('molink_keyboardGrab') === '1',
    // On by default: a desktop browser needs the hidden input for IME
    // composition. Turning it off returns to position-only key forwarding.
    localIme: readStorage(LOCAL_IME_KEY) !== '0',
    comboForwardAsked: normalizeComboForwardAsked(
      readStorage(COMBO_FORWARD_KEY),
    ),
    comboForwardAnswered: false,
    windowCloseBehavior: (() => {
      const saved = readStorage(CLOSE_BEHAVIOR_KEY)
      return saved === 'tray' || saved === 'tab' ? saved : 'ask'
    })(),
    defaultPreset: initialDefaultPreset(),
    presets: initialPresets(),
    advancedQuality: readStorage(ADVANCED_QUALITY_KEY) === '1',
    advanced: initialAdvanced(),
    qualityPanelOpen: false,
    settingsOpen: false,
    windowFullscreen: false,
    immersive: false,
    historyView: initialHistoryView(),
    preferredMode: initialSessionMode(),
    forceRelay: readStorage('molink.forceRelay') === '1',
    twoFactorCode: '',
    uiMode: initialUiMode(),
    savedDevices: initialSavedDevices(),
    deviceNotes: initialDeviceNotes(),
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
    panes: [],
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

    setWatermarkFade: (enabled) => {
      writeStorage(WATERMARK_FADE_KEY, enabled ? '1' : '0')
      set({ watermarkFade: enabled })
    },

    setAutoAudio: (enabled) => {
      writeStorage(AUTO_AUDIO_KEY, enabled ? '1' : '0')
      set({ autoAudio: enabled })
    },

    setRemoteClipboardFiles: (enabled) => {
      writeStorage(REMOTE_CLIPBOARD_FILES_KEY, enabled ? '1' : '0')
      set({ remoteClipboardFiles: enabled })
      for (const connection of runtimes.values()) {
        connection.setRemoteClipboardEnabled(enabled)
      }
    },

    setClipboardVirtualFiles: (enabled) => {
      writeStorage(CLIPBOARD_VIRTUAL_FILES_KEY, enabled ? '1' : '0')
      set({ clipboardVirtualFiles: enabled })
      // Each announcement re-reads the setting; turning it off also releases
      // a mount that is already on this machine's clipboard.
      for (const connection of runtimes.values()) {
        connection.setClipboardVirtualEnabled(enabled)
      }
    },

    setClipboardOffer: (id, prompt) => {
      patchTab(id, { clipboardOffer: prompt })
    },

    setTabToast: (id, toast) => {
      if (toast === null) clearToastTimer(id)
      patchTab(id, { toast })
    },

    pushToast,

    setRememberDevice: (enabled) => {
      writeStorage(REMEMBER_DEVICE_KEY, enabled ? '1' : '0')
      set({ rememberDevice: enabled })
      // Turning it off deletes every persisted session credential; the
      // in-memory copy survives so the open tab can still reconnect.
      sessionStore.setRememberDevice(enabled)
      schedulePersist()
    },

    setKeyboardGrab: (enabled) => {
      writeStorage(KEYBOARD_GRAB_KEY, enabled ? '1' : '0')
      writeCookie('molink_keyboardGrab', enabled ? '1' : '0')
      set({ keyboardGrab: enabled })
    },

    setLocalIme: (enabled) => {
      writeStorage(LOCAL_IME_KEY, enabled ? '1' : '0')
      set({ localIme: enabled })
      // The native shell's origin changes on every launch; ui.json is the
      // copy that survives it.
      schedulePersist()
    },

    setComboForwardAsked: (choice) => {
      if (choice === null) {
        try {
          window.localStorage.removeItem(COMBO_FORWARD_KEY)
        } catch {
          // Storage can be disabled; the in-memory choice still applies.
        }
      } else {
        writeStorage(COMBO_FORWARD_KEY, choice)
      }
      set({ comboForwardAsked: choice })
      // The native shell's origin changes on every launch; ui.json is the
      // copy that survives it.
      schedulePersist()
    },

    markComboForwardAnswered: () => {
      set({ comboForwardAnswered: true })
    },

    setWindowCloseBehavior: (behavior) => {
      writeStorage(CLOSE_BEHAVIOR_KEY, behavior)
      set({ windowCloseBehavior: behavior })
    },

    setDefaultPreset: (preset) => {
      writeStorage(DEFAULT_PRESET_KEY, preset)
      set({ defaultPreset: preset })
      // The settings page is host-local; mirror the default into ui.json so a
      // native-shell restart (new origin) keeps it.
      schedulePersist()
    },

    setTabPreset: (id, preset) => {
      const tab = get().tabs.find((item) => item.id === id)
      if (!tab) return
      const key = presetStorageKey(tab.host, tab.port)
      const stored: StoredSessionPreset = {
        preset,
        updatedAt: Date.now(),
      }
      writeStoredPreset(tab.host, tab.port, stored)
      set((state) => ({
        presets: { ...state.presets, [key]: stored },
        tabs: state.tabs.map((item) =>
          item.id === id ? { ...item, preset, appliedPreset: undefined } : item,
        ),
      }))
      schedulePersist()
    },

    setAdvancedQuality: (enabled) => {
      writeStorage(ADVANCED_QUALITY_KEY, enabled ? '1' : '0')
      set({ advancedQuality: enabled })
      schedulePersist()
    },

    setTabAdvanced: (id, patch) => {
      const tab = get().tabs.find((item) => item.id === id)
      if (!tab) return
      const current = tab.advanced ?? defaultAdvancedParams(tab.preset)
      const next = normalizeAdvancedParams({ ...current, ...patch }, current)
      const key = advancedStorageKey(tab.host, tab.port)
      writeStoredAdvanced(tab.host, tab.port, next)
      set((state) => ({
        advanced: { ...state.advanced, [key]: next },
        tabs: state.tabs.map((item) =>
          item.id === id ? { ...item, advanced: next } : item,
        ),
      }))
      schedulePersist()
    },

    setQualityPanelOpen: (open) => set({ qualityPanelOpen: open }),

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
    setTwoFactorCode: (value: string) => {
      set({ twoFactorCode: value })
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
        const notes = sanitizeDeviceNotes(prefs.deviceNotes)
        if (Object.keys(notes).length > 0) {
          next.deviceNotes = { ...get().deviceNotes, ...notes }
          writeStorage(
            DEVICE_NOTES_KEY,
            JSON.stringify(next.deviceNotes),
          )
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
        // Viewer-owned presets, keyed by `molink.preset.<host>:<port>`. The
        // ui.json copy is the one that survives the shell's ephemeral origin.
        const storedPresets: Record<string, StoredSessionPreset> = {}
        if (prefs.presets && typeof prefs.presets === 'object') {
          for (const [key, value] of Object.entries(prefs.presets)) {
            if (!key.startsWith(PRESET_STORAGE_PREFIX)) continue
            const parsed = parseStoredPreset(JSON.stringify(value))
            if (!parsed) continue
            storedPresets[key] = parsed
            try {
              window.localStorage.setItem(key, JSON.stringify(parsed))
            } catch {
              // Blocked storage; the in-memory map still applies.
            }
          }
        }
        if (Object.keys(storedPresets).length > 0) {
          next.presets = { ...get().presets, ...storedPresets }
        }
        // The advanced panel's per-device parameters use the same ui.json
        // mirror, keyed by `molink.advanced.<host>:<port>`.
        const storedAdvanced: Record<string, AdvancedQualityParams> = {}
        if (prefs.advanced && typeof prefs.advanced === 'object') {
          for (const [key, value] of Object.entries(prefs.advanced)) {
            if (!key.startsWith(ADVANCED_STORAGE_PREFIX)) continue
            const parsed = parseStoredAdvanced(JSON.stringify(value))
            if (!parsed) continue
            storedAdvanced[key] = parsed
            try {
              window.localStorage.setItem(key, JSON.stringify(parsed))
            } catch {
              // Blocked storage; the in-memory map still applies.
            }
          }
        }
        if (Object.keys(storedAdvanced).length > 0) {
          next.advanced = { ...get().advanced, ...storedAdvanced }
        }
        if (typeof prefs.defaultPreset === 'string') {
          next.defaultPreset = normalizePresetId(prefs.defaultPreset)
          writeStorage(DEFAULT_PRESET_KEY, next.defaultPreset)
        }
        if (typeof prefs.advancedQuality === 'boolean') {
          next.advancedQuality = prefs.advancedQuality
          writeStorage(ADVANCED_QUALITY_KEY, prefs.advancedQuality ? '1' : '0')
        }
        // A remembered combo-forwarding answer survives the shell's changing
        // origin through ui.json. An absent value leaves the local copy alone
        // (first run, or an older ui.json).
        const comboAsked = normalizeComboForwardAsked(prefs.comboForwardAsked)
        if (comboAsked !== null) {
          next.comboForwardAsked = comboAsked
          writeStorage(COMBO_FORWARD_KEY, comboAsked)
        }
        if (typeof prefs.localIme === 'boolean') {
          next.localIme = prefs.localIme
          writeStorage(LOCAL_IME_KEY, prefs.localIme ? '1' : '0')
        }
        // The core is the source of truth for remember_device and the session
        // tokens in ui.json. Apply both together so turning the switch off on
        // one launch cannot leave an old credential usable on the next.
        const remember =
          typeof prefs.rememberDevice === 'boolean'
            ? prefs.rememberDevice
            : get().rememberDevice
        sessionStore.applyStoredPreferences(remember, prefs.sessions)
        writeStorage(REMEMBER_DEVICE_KEY, remember ? '1' : '0')
        next.rememberDevice = remember
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

    setDeviceNote: (name, note) => {
      const key = deviceNoteKey(name)
      if (!key) return
      const value = cleanDeviceNote(note)
      const notes = { ...get().deviceNotes }
      if (value) {
        notes[key] = value
      } else if (!(key in notes)) {
        return
      } else {
        delete notes[key]
      }
      writeStorage(DEVICE_NOTES_KEY, JSON.stringify(notes))
      set({ deviceNotes: notes })
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
        // Choose where reconnect credentials live before anything can save
        // one: ui.json for the local shell, localStorage for a remote browser.
        configureSessionBackend(state.local)
        if (state.locked) {
          set({ authPhase: 'locked' })
          return
        }
        if (state.local) {
          try {
            const device = await fetchDeviceInfo()
            set({ device, token: device.token })
            configureSessionBackend(true)
            // Hydrate the persisted credentials before the home screen lets
            // the user connect.
            await get().loadUiPreferences()
            set({ authPhase: 'ready', stateError: null })
            get().syncUiState()
            void get().refreshSecurity()
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
        configureSessionBackend(state.local)
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
            set({ device, token: device.token })
            configureSessionBackend(true)
            await get().loadUiPreferences()
            set({ authPhase: 'ready' })
            get().syncUiState()
            void get().refreshSecurity()
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
          configureSessionBackend(true)
          try {
            const device = await fetchDeviceInfo()
            set({ device, token: device.token })
          } catch {
            // keep the login result
          }
          await get().loadUiPreferences()
          void get().refreshSecurity()
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

    setCodePolicy: async (single, multi) => {
      const token = get().token
      if (!token) return null
      try {
        const code = await postCodePolicy(token, single, multi)
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
      const trimmedHost = host.trim()
      const presetKey = presetStorageKey(trimmedHost, port)
      const preset =
        get().presets[presetKey]?.preset ?? get().defaultPreset
      const advancedKey = advancedStorageKey(trimmedHost, port)
      const advanced =
        get().advanced[advancedKey] ??
        readStoredAdvanced(trimmedHost, port) ??
        defaultAdvancedParams(preset)
      // A files/chat/terminal tab is one independent page: create its pane
      // right away so the app-level PaneHost can mount it once. Desktop tabs
      // create panes only when a drawer is opened.
      const paneId =
        intent === 'desktop' ? undefined : `pane-${nextPaneId++}`
      const tab: SessionTab = {
        id,
        label: label?.trim() || host,
        host: trimmedHost,
        port,
        token: trimmedToken,
        deviceId,
        preset,
        advanced,
        runtimeTabId: id,
        ...(paneId ? { paneId } : {}),
        phase: 'connecting',
        welcome: null,
        frame: null,
        cursorShape: null,
        cursorPosition: null,
        stats: EMPTY_STATS,
        error: null,
        notice: null,
        toast: null,
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
        panes:
          paneId && intent !== 'desktop'
            ? [
                ...state.panes,
                {
                  id: paneId,
                  kind: intent,
                  ownerTabId: id,
                  standaloneTabId: id,
                },
              ]
            : state.panes,
        activeTabId: id,
      }))
      logDiag('info', 'session.create', tab.label, {
        target: `${tab.host}:${tab.port}`,
        mode: tab.mode,
        intent: tab.intent,
        access: requestAccess || undefined,
        device: deviceId || undefined,
      })
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
            // A local clipboard failure is not a session failure: the link is
            // healthy, so show a transient toast instead of the red banner.
            pushToast(id, 'clipboard_unavailable', 'warn')
            return
          }
          // A rejected write (unfocused or permissionless browser) is not
          // actionable; do not cover the session with an error banner.
          navigator.clipboard.writeText(text).catch(() => undefined)
        },
        onFailure: (message, code, tone) => {
          reportFailure(id, message, code ?? null, tone)
        },
      }, id)
      runtimes.set(id, connection)
      // A files/chat/terminal pane can be mounted by PaneHost before this
      // point (the tab was added in the same action). Its first render saw no
      // connection; nudge the store so it renders again with the runtime.
      patchTab(id, {})
      setForegroundRuntime(id)
      if (get().localMode && intent !== 'terminal') {
        // Local files go to every remote; the local core's clipboard API is
        // only reachable when this UI is the local shell/browser. A terminal
        // page has no remote picture to paste into.
        connection.enableClipboardFileSync(() => get().token)
        // And the reverse: files copied on the remote can be put on this
        // machine's clipboard (mounted virtually when possible).
        connection.enableRemoteClipboardSync(
          () => get().token,
          get().remoteClipboardFiles,
          {
            // Mount with FUSE when this core can and the user allows it.
            useVirtual: () =>
              get().clipboardVirtualFiles &&
              get().device?.capabilities.clipboard_virtual === true,
            // This core has the setting on but cannot publish virtual files:
            // say so instead of silently falling back to a full download.
            noteVirtualFallback: () =>
              get().clipboardVirtualFiles &&
              (get().device?.platform === 'linux' ||
                get().device?.platform === 'windows') &&
              get().device?.capabilities.clipboard_virtual !== true,
            onPrompt: (prompt) => patchTab(id, { clipboardOffer: prompt }),
          },
        )
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
                           clientName, clientId, self.twoFactorCode || '',
                           intent)
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
      const closing = get().tabs.find((tab) => tab.id === id)
      if (!closing) return
      logDiag('info', 'session.close', closing.label, {
        target: `${closing.host}:${closing.port}`,
        intent: closing.intent,
      })
      const runtimeId = closing.runtimeTabId ?? id
      clearToastTimer(id)
      let disconnect: SessionConnection | undefined
      set((state) => {
        const index = state.tabs.findIndex((tab) => tab.id === id)
        const tabs = state.tabs.filter((tab) => tab.id !== id)
        // A pane that lived in this tab goes with it. A promoted pane whose
        // owner closes keeps its own tab and the shared runtime alive.
        const panes = state.panes.filter((pane) => {
          if (pane.id === closing.paneId) return false
          if (pane.ownerTabId !== id) return true
          return pane.standaloneTabId !== undefined &&
                 tabs.some((tab) => tab.id === pane.standaloneTabId)
        })
        const runtimeStillUsed =
          tabs.some((tab) => (tab.runtimeTabId ?? tab.id) === runtimeId) ||
          panes.some((pane) => pane.ownerTabId === runtimeId)
        if (!runtimeStillUsed) {
          disconnect = runtimes.get(runtimeId) ?? runtimes.get(id)
          runtimes.delete(runtimeId)
          runtimes.delete(id)
        }
        let activeTabId = state.activeTabId
        if (activeTabId === id) {
          const candidates = tabs.filter((tab) => !tab.minimized)
          const neighbour = candidates[Math.max(0, Math.min(index - 1, candidates.length - 1))]
          activeTabId = neighbour?.id ?? 'home'
        }
        return { tabs, panes, activeTabId }
      })
      disconnect?.disconnect()
      if (closing.intent === 'desktop') forgetSessionScreenBuffer(id)
      setForegroundRuntime(get().activeTabId)
      get().syncUiState()
    },

    setActiveTab: (id) => {
      set((state) => ({
        activeTabId: id,
        tabs: state.tabs.map((tab) =>
          tab.id === id && tab.minimized ? { ...tab, minimized: false } : tab,
        ),
      }))
      setForegroundRuntime(id)
    },

    setTabMinimized: (id, minimized) => {
      set((state) => {
        const tabs = state.tabs.map((tab) =>
          tab.id === id ? { ...tab, minimized } : tab,
        )
        let activeTabId = state.activeTabId
        if (minimized && activeTabId === id) {
          activeTabId = nextVisibleTabId(tabs, id)
        } else if (!minimized) {
          activeTabId = id
        }
        return { tabs, activeTabId }
      })
      setForegroundRuntime(get().activeTabId)
    },

    openPane: (ownerTabId, kind) => {
      const current = get().panes.find(
        (pane) => pane.ownerTabId === ownerTabId && pane.kind === kind,
      )
      // A pane already promoted to its own tab is reached through that tab.
      if (current?.standaloneTabId) {
        get().setActiveTab(current.standaloneTabId)
        return current.id
      }
      if (current) {
        get().setActiveTab(ownerTabId)
        return current.id
      }
      const id = `pane-${nextPaneId++}`
      set((state) => ({
        panes: [
          // The desktop drawer is exclusive, like before: opening files
          // while chat is shown replaces it.
          ...state.panes.filter(
            (pane) =>
              pane.ownerTabId !== ownerTabId || pane.standaloneTabId !== undefined,
          ),
          { id, kind, ownerTabId },
        ],
        activeTabId: ownerTabId,
      }))
      return id
    },

    closePane: (id) => {
      const pane = get().panes.find((item) => item.id === id)
      if (!pane) return
      if (
        pane.standaloneTabId !== undefined &&
        get().tabs.some((tab) => tab.id === pane.standaloneTabId)
      ) {
        get().closeTab(pane.standaloneTabId)
        return
      }
      set((state) => ({ panes: state.panes.filter((item) => item.id !== id) }))
    },

    promotePane: (id) => {
      const pane = get().panes.find((item) => item.id === id)
      if (!pane) return null
      if (
        pane.standaloneTabId !== undefined &&
        get().tabs.some((tab) => tab.id === pane.standaloneTabId)
      ) {
        get().setActiveTab(pane.standaloneTabId)
        return pane.standaloneTabId
      }
      const owner = get().tabs.find((tab) => tab.id === pane.ownerTabId)
      if (!owner) return null
      const tabId = `session-${nextId++}`
      const runtimeId = owner.runtimeTabId ?? owner.id
      const tab: SessionTab = {
        ...owner,
        id: tabId,
        intent: pane.kind,
        runtimeTabId: runtimeId,
        paneId: pane.id,
        minimized: false,
        // The picture tabs' input state is irrelevant on a panel page.
        controlEnabled: false,
        // The promoted page shares the owner's connection; it is already
        // live, so no reconnect and no second hello.
        phase: owner.phase,
        error: owner.error,
        errorCode: owner.errorCode,
        notice: owner.notice,
      }
      set((state) => ({
        tabs: [...state.tabs, tab],
        panes: state.panes.map((item) =>
          item.id === id ? { ...item, standaloneTabId: tabId } : item,
        ),
        activeTabId: tabId,
      }))
      logDiag('info', 'session.promote', owner.label, {
        target: `${owner.host}:${owner.port}`,
        intent: pane.kind,
        runtime: runtimeId,
      })
      setForegroundRuntime(tabId)
      return tabId
    },

    setControlEnabled: (id, enabled) =>
      patchTab(id, { controlEnabled: enabled }),

    setClipboardEnabled: (id, enabled) => {
      patchTab(id, { clipboardEnabled: enabled })
      runtimes.get(id)?.setClipboardEnabled(enabled)
    },

    forgetRecent: (at) => {
      // Removing a device clears every row that refers to it, not just the one
      // that was clicked: the same machine can have older rows under another
      // address.
      const target = get().recents.find((item) => item.at === at)
      const recents = get().recents.filter((item) =>
        target === undefined
          ? item.at !== at
          : !(
              (target.id !== undefined && item.id === target.id) ||
              (item.host === target.host && item.port === target.port)
            ),
      )
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
