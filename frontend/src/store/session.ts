import { create } from 'zustand'
import { MolinkClient, type ClientPhase, type Welcome } from '../lib/client'
import type { Frame } from '../lib/protocol'

export const client = new MolinkClient()

const TOKEN_KEY = 'molink.token'

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
    // Private windows and blocked storage are fine: the token just is not
    // remembered for next time.
  }
}

const params = new URLSearchParams(window.location.search)

export interface Stats {
  fps: number
  latencyMs: number | null
  bytesPerSec: number
  frames: number
}

export interface SessionState {
  phase: ClientPhase
  host: string
  port: string
  token: string
  error: string | null
  welcome: Welcome | null
  frame: Frame | null
  stats: Stats
  setField: (field: 'host' | 'port' | 'token', value: string) => void
  connect: () => void
  disconnect: () => void
}

const emptyStats = (): Stats => ({
  fps: 0,
  latencyMs: null,
  bytesPerSec: 0,
  frames: 0,
})

// Sliding one-second windows live outside React state: they change per frame
// and should not trigger a re-render by themselves.
let frameTimes: number[] = []
let byteWindow: { t: number; n: number }[] = []
let latencyMs: number | null = null
let frames = 0

function resetWindows(): void {
  frameTimes = []
  byteWindow = []
  latencyMs = null
  frames = 0
}

export const useSessionStore = create<SessionState>((set, get) => ({
  phase: 'idle',
  host: params.get('host') ?? '127.0.0.1',
  port: params.get('port') ?? '21118',
  token: params.get('token') ?? readStorage(TOKEN_KEY),
  error: null,
  welcome: null,
  frame: null,
  stats: emptyStats(),

  setField: (field, value) => set({ [field]: value } as Partial<SessionState>),

  connect: () => {
    const { host, port, token } = get()
    const portNumber = Number(port)
    if (!Number.isInteger(portNumber) || portNumber < 1 || portNumber > 65535) {
      set({ error: 'invalid_port', phase: 'error' })
      return
    }
    if (token.trim() === '') {
      set({ error: 'no_token', phase: 'error' })
      return
    }
    writeStorage(TOKEN_KEY, token.trim())
    resetWindows()
    set({
      phase: 'connecting',
      error: null,
      welcome: null,
      frame: null,
      stats: emptyStats(),
    })
    client.connect(host.trim(), portNumber, token.trim())
  },

  disconnect: () => {
    client.disconnect()
    resetWindows()
    set({ phase: 'idle', frame: null, welcome: null, stats: emptyStats() })
  },
}))

client.setHandlers({
  onPhase: (phase, detail) => {
    if (phase === 'live') {
      useSessionStore.setState({ phase, error: null })
      return
    }
    if (phase === 'error') {
      useSessionStore.setState({
        phase,
        error: detail ?? 'connection failed',
        frame: null,
      })
      return
    }
    useSessionStore.setState({ phase })
  },

  onWelcome: (welcome) => {
    useSessionStore.setState({ welcome })
  },

  onFrame: (frame, byteLength) => {
    const now = performance.now()
    frames += 1
    frameTimes.push(now)
    byteWindow.push({ t: now, n: byteLength })
    while (frameTimes.length > 0 && now - frameTimes[0]! > 1000) {
      frameTimes.shift()
    }
    while (byteWindow.length > 0 && now - byteWindow[0]!.t > 1000) {
      byteWindow.shift()
    }
    const bytesPerSec = byteWindow.reduce((sum, item) => sum + item.n, 0)
    useSessionStore.setState({
      frame,
      stats: {
        fps: frameTimes.length,
        latencyMs,
        bytesPerSec,
        frames,
      },
    })
  },

  onLatency: (ms) => {
    latencyMs = ms
    const current = useSessionStore.getState().stats
    useSessionStore.setState({ stats: { ...current, latencyMs: ms } })
  },

  onError: (message) => {
    useSessionStore.setState({ error: message })
  },
})

// A token in the query string means the shell (or a deep link) already knows
// where to connect. Connect once, before the first render, so the session view
// is what the user sees.
if (params.get('token')) {
  queueMicrotask(() => {
    useSessionStore.getState().connect()
  })
}
