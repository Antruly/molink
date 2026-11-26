/// Remote terminal drawer.
///
/// One WebSocket carries every tab in the session: the host's `/ws/terminal`
/// endpoint keeps a viewer identity per connection and allows four live
/// terminals for it, so opening a second socket would halve that budget. Each
/// tab has its own host-side id, byte counter (`seq`) and xterm instance.
///
/// The connection is independent from the desktop session's WebSocket: a
/// terminal process keeps running when this drawer is hidden or the network
/// drops. `term-attach` replays the host's 1 MiB ring from the last byte the
/// page rendered; the pure dedupe helper drops the overlap.
import { FitAddon } from '@xterm/addon-fit'
import { Terminal } from '@xterm/xterm'
import '@xterm/xterm/css/xterm.css'
import { Plus, Terminal as TerminalIcon, X } from 'lucide-react'
import {
  useCallback,
  useEffect,
  useRef,
  useState,
} from 'react'
import { useTranslation } from 'react-i18next'
import {
  applyOutputFrame,
  createResizeDebouncer,
  decodeControl,
  decodeOutputFrame,
  encodeControl,
  encodeInputFrame,
  terminalAttach,
  terminalClose,
  terminalOpen,
  terminalResize,
  terminalSocketUrl,
  type ResizeDebouncer,
  type TerminalControl,
} from '../lib/terminal'

const MAX_TERMINALS = 4
const DEFAULT_COLS = 80
const DEFAULT_ROWS = 24
/// Follow the platform's own monospace stack (Cascadia/Consolas on Windows,
/// Ubuntu/DejaVu/Noto on Linux) instead of the browser default, which lands on
/// Liberation Mono on many Linux desktop themes. No web fonts: the terminal
/// must never wait on the network for glyphs.
const TERMINAL_FONT_FAMILY =
  '"Cascadia Mono", "Consolas", "Ubuntu Mono", "DejaVu Sans Mono", "Noto Sans Mono", "Liberation Mono", monospace'
const TERMINAL_FONT_SIZE = 13
const encoder = new TextEncoder()

type ConnectionState =
  | 'connecting'
  | 'open'
  | 'closed'
  | 'fatal'
  | 'disabled'

type TabPhase = 'opening' | 'open' | 'exited' | 'lost'

interface TabState {
  key: number
  /// Label number, stable for the lifetime of the tab.
  n: number
  id: number | null
  phase: TabPhase
  exitCode: number | null
  cols: number
  rows: number
}

interface Size {
  cols: number
  rows: number
}

/// One xterm instance. It lives for as long as the tab id owns the same host
/// terminal; hiding the tab keeps its scrollback and cursor.
function TerminalTabView({
  terminalId,
  active,
  onInput,
  onResize,
  registerWriter,
}: {
  terminalId: number
  active: boolean
  onInput: (id: number, data: string) => void
  onResize: (id: number, cols: number, rows: number) => void
  registerWriter: (
    id: number,
    write: ((data: Uint8Array) => void) | null,
  ) => void
}) {
  const hostRef = useRef<HTMLDivElement | null>(null)
  const termRef = useRef<Terminal | null>(null)
  const fitRef = useRef<FitAddon | null>(null)

  useEffect(() => {
    const host = hostRef.current
    if (host === null) return
    const term = new Terminal({
      cursorBlink: true,
      fontSize: TERMINAL_FONT_SIZE,
      fontFamily: TERMINAL_FONT_FAMILY,
      lineHeight: 1.2,
      letterSpacing: 0,
      scrollback: 5000,
      theme: {
        background: '#0b0e14',
        foreground: '#d6deeb',
        cursor: '#7aa2f7',
        selectionBackground: '#2c3a58',
      },
    })
    const fit = new FitAddon()
    term.loadAddon(fit)
    term.open(host)
    termRef.current = term
    fitRef.current = fit
    const dataSub = term.onData((data) => onInput(terminalId, data))
    registerWriter(terminalId, (data) => term.write(data))

    const fitNow = () => {
      // A hidden tab has no measurable cell; fitting it would throw (or
      // produce a 1x1 grid), so wait for the tab to become active.
      if (host.clientWidth === 0 || host.clientHeight === 0) return
      try {
        fit.fit()
      } catch {
        return
      }
      onResize(terminalId, term.cols, term.rows)
    }
    const observer =
      typeof ResizeObserver === 'undefined'
        ? null
        : new ResizeObserver(fitNow)
    observer?.observe(host)
    fitNow()

    return () => {
      observer?.disconnect()
      dataSub.dispose()
      registerWriter(terminalId, null)
      term.dispose()
      termRef.current = null
      fitRef.current = null
    }
  }, [terminalId, onInput, onResize, registerWriter])

  useEffect(() => {
    if (!active) return
    const term = termRef.current
    const fit = fitRef.current
    const host = hostRef.current
    if (term === null || fit === null || host === null) return
    // The wrapper was `display: none` a moment ago; layout is available now,
    // but wait for the browser to actually paint the new size.
    const frame = requestAnimationFrame(() => {
      try {
        fit.fit()
      } catch {
        return
      }
      onResize(terminalId, term.cols, term.rows)
      term.focus()
    })
    return () => cancelAnimationFrame(frame)
  }, [active, terminalId, onResize])

  return <div ref={hostRef} className="h-full w-full" />
}

export function TerminalPanel({
  host,
  port,
  token,
  sessionToken,
  onClose,
  onDisabled,
}: {
  host: string
  port: number
  /// Session credential from the host's welcome; reusable, unlike a spent
  /// single-use code. `token` is the fallback for an older host.
  sessionToken?: string
  token: string
  onClose: () => void
  /// The host has `terminal_enabled=false`. The session hides the toolbar
  /// entry so it cannot be clicked again.
  onDisabled?: () => void
}) {
  const { t } = useTranslation()
  const [connection, setConnection] = useState<ConnectionState>('connecting')
  const [tabs, setTabs] = useState<TabState[]>([])
  const [activeKey, setActiveKey] = useState(0)
  const [notice, setNotice] = useState<string | null>(null)

  const wsRef = useRef<WebSocket | null>(null)
  const tabsRef = useRef<TabState[]>([])
  const activeKeyRef = useRef(0)
  const nextKeyRef = useRef(1)
  const nextLabelRef = useRef(1)
  /// Tab key whose `term-open` is waiting for its `term-opened` reply.
  const pendingOpenRef = useRef<number | null>(null)
  /// Byte counters: id -> bytes rendered (the `seq` an attach must send).
  const seqRef = useRef(new Map<number, number>())
  /// Last fitted geometry per id, resent after a reconnect because the host
  /// keeps the old size while the socket was gone.
  const sizeRef = useRef(new Map<number, Size>())
  const writersRef = useRef(new Map<number, (data: Uint8Array) => void>())
  /// Output that arrived between `term-opened` and the tab's React commit.
  const queuedRef = useRef(new Map<number, Uint8Array[]>())
  const resizersRef = useRef(new Map<number, ResizeDebouncer>())
  /// Ids whose `term-attach` is in flight; an ambiguous `term_not_found`
  /// error belongs to the first one.
  const attachingRef = useRef<number[]>([])
  const disabledRef = useRef(false)
  const fatalRef = useRef(false)
  const tRef = useRef(t)
  const onDisabledRef = useRef(onDisabled)
  /// Connection target captured once. The session's `welcome` (and therefore
  /// `sessionToken`) can go null for a moment during its own reconnect; the
  /// terminal socket must not be torn down for that.
  const targetRef = useRef({ host, port, token, sessionToken })

  useEffect(() => {
    tRef.current = t
    onDisabledRef.current = onDisabled
    // The session's welcome can briefly go null while its own socket
    // reconnects; keep the last usable credential instead of falling back to
    // a single-use code that was already spent.
    const previous = targetRef.current
    targetRef.current = {
      host,
      port,
      token: token !== '' ? token : previous.token,
      sessionToken:
        sessionToken !== undefined && sessionToken !== ''
          ? sessionToken
          : previous.sessionToken,
    }
  })

  const commitTabs = useCallback((next: TabState[]) => {
    tabsRef.current = next
    setTabs(next)
  }, [])

  const selectTab = useCallback((key: number) => {
    activeKeyRef.current = key
    setActiveKey(key)
  }, [])

  const sendControl = useCallback((message: TerminalControl): boolean => {
    const ws = wsRef.current
    if (ws === null || ws.readyState !== WebSocket.OPEN) return false
    ws.send(encodeControl(message))
    return true
  }, [])

  const writeToTerminal = useCallback((id: number, data: Uint8Array) => {
    const write = writersRef.current.get(id)
    if (write !== undefined) {
      write(data)
      return
    }
    const queue = queuedRef.current.get(id)
    if (queue === undefined) {
      queuedRef.current.set(id, [data])
    } else {
      queue.push(data)
    }
  }, [])

  const registerWriter = useCallback(
    (id: number, write: ((data: Uint8Array) => void) | null) => {
      if (write === null) {
        writersRef.current.delete(id)
        return
      }
      writersRef.current.set(id, write)
      const queue = queuedRef.current.get(id)
      if (queue === undefined) return
      queuedRef.current.delete(id)
      for (const data of queue) write(data)
    },
    [],
  )

  const handleInput = useCallback((id: number, data: string) => {
    const tab = tabsRef.current.find((item) => item.id === id)
    if (tab === undefined || tab.phase !== 'open') return
    const ws = wsRef.current
    if (ws === null || ws.readyState !== WebSocket.OPEN) return
    ws.send(encodeInputFrame(id, encoder.encode(data)))
  }, [])

  const handleResize = useCallback(
    (id: number, cols: number, rows: number) => {
      sizeRef.current.set(id, { cols, rows })
      let resizer = resizersRef.current.get(id)
      if (resizer === undefined) {
        resizer = createResizeDebouncer((nextCols, nextRows) => {
          sendControl(terminalResize(id, nextCols, nextRows))
        })
        resizersRef.current.set(id, resizer)
      }
      resizer.update(cols, rows)
    },
    [sendControl],
  )

  const openTab = useCallback(() => {
    if (tabsRef.current.length >= MAX_TERMINALS) {
      setNotice(tRef.current('terminal.limit'))
      return
    }
    if (pendingOpenRef.current !== null) return
    const key = nextKeyRef.current++
    const n = nextLabelRef.current++
    const tab: TabState = {
      key,
      n,
      id: null,
      phase: 'opening',
      exitCode: null,
      cols: DEFAULT_COLS,
      rows: DEFAULT_ROWS,
    }
    pendingOpenRef.current = key
    setNotice(null)
    commitTabs([...tabsRef.current, tab])
    selectTab(key)
    sendControl(terminalOpen(tab.cols, tab.rows))
  }, [commitTabs, selectTab, sendControl])

  const closeTab = useCallback(
    (key: number) => {
      const tab = tabsRef.current.find((item) => item.key === key)
      if (tab === undefined) return
      if (
        tab.id !== null &&
        tab.phase !== 'exited' &&
        tab.phase !== 'lost'
      ) {
        sendControl(terminalClose(tab.id))
      }
      if (tab.id !== null) {
        seqRef.current.delete(tab.id)
        sizeRef.current.delete(tab.id)
        queuedRef.current.delete(tab.id)
        resizersRef.current.get(tab.id)?.cancel()
        resizersRef.current.delete(tab.id)
      }
      if (pendingOpenRef.current === key) pendingOpenRef.current = null
      const next = tabsRef.current.filter((item) => item.key !== key)
      commitTabs(next)
      if (activeKeyRef.current === key) {
        selectTab(next.length > 0 ? next[next.length - 1].key : 0)
      }
    },
    [commitTabs, selectTab, sendControl],
  )

  const handleExit = useCallback(
    (id: number, code: number) => {
      const tab = tabsRef.current.find((item) => item.id === id)
      // A tab closed while the exit was in flight has no xterm to write to.
      if (tab === undefined) return
      commitTabs(
        tabsRef.current.map((item) =>
          item.id === id
            ? { ...item, phase: 'exited', exitCode: code }
            : item,
        ),
      )
      const line = `\r\n${tRef.current('terminal.exit', { code })}\r\n`
      writeToTerminal(id, encoder.encode(line))
    },
    [commitTabs, writeToTerminal],
  )

  const handleControl = useCallback(
    (message: TerminalControl) => {
      switch (message.type) {
        case 'term-opened': {
          const key = pendingOpenRef.current
          if (key === null) {
            // The tab was closed while its open was in flight. The host has
            // already spawned it; close it instead of leaking a process.
            sendControl(terminalClose(message.id))
            return
          }
          pendingOpenRef.current = null
          seqRef.current.set(message.id, 0)
          setNotice(null)
          commitTabs(
            tabsRef.current.map((tab) =>
              tab.key === key
                ? { ...tab, id: message.id, phase: 'open' }
                : tab,
            ),
          )
          return
        }
        case 'term-attached': {
          // The attach is answered; stop treating a later id-less
          // term_not_found as this tab's.
          const index = attachingRef.current.indexOf(message.id)
          if (index >= 0) attachingRef.current.splice(index, 1)
          // The tab may have been closed while the attach was in flight; its
          // writer is gone and no notice belongs anywhere.
          if (
            message.truncated &&
            tabsRef.current.some((tab) => tab.id === message.id)
          ) {
            // The host's ring no longer held every byte this page asked for.
            // Say so on the terminal itself, before the replay frames arrive.
            const line = `\r\n${tRef.current('terminal.truncated')}\r\n`
            writeToTerminal(message.id, encoder.encode(line))
          }
          return
        }
        case 'term-exit':
          handleExit(message.id, message.code)
          return
        case 'error': {
          if (message.code === 'terminal_disabled') {
            fatalRef.current = true
            disabledRef.current = true
            setConnection('disabled')
            setNotice(tRef.current('terminal.disabled'))
            onDisabledRef.current?.()
            wsRef.current?.close()
            return
          }
          if (message.code === 'unauthorized') {
            fatalRef.current = true
            setConnection('fatal')
            setNotice(tRef.current('terminal.unauthorized'))
            wsRef.current?.close()
            return
          }
          // A newer host names the terminal in the error. Older hosts sent
          // no id; fall back to the order attaches were issued in.
          let errorId = message.id
          if (errorId !== undefined) {
            const index = attachingRef.current.indexOf(errorId)
            if (index >= 0) attachingRef.current.splice(index, 1)
          } else if (
            message.code === 'term_not_found' &&
            attachingRef.current.length > 0
          ) {
            errorId = attachingRef.current.shift()
          }
          if (message.code === 'term_not_found' && errorId !== undefined) {
            const lost = tabsRef.current.find((tab) => tab.id === errorId)
            if (lost !== undefined) {
              selectTab(lost.key)
              commitTabs(
                tabsRef.current.map((tab) =>
                  tab.id === errorId ? { ...tab, phase: 'lost' } : tab,
                ),
              )
              setNotice(tRef.current('terminal.lost'))
              return
            }
          }
          if (errorId !== undefined) {
            const tab = tabsRef.current.find((item) => item.id === errorId)
            if (tab !== undefined) selectTab(tab.key)
          }
          const detail = message.message ?? message.code
          if (message.code === 'term_limit' ||
              message.code === 'terminal_open_failed' ||
              message.code === 'terminal_unavailable') {
            const key = pendingOpenRef.current
            if (key !== null) {
              pendingOpenRef.current = null
              commitTabs(
                tabsRef.current.map((tab) =>
                  tab.key === key ? { ...tab, phase: 'lost' } : tab,
                ),
              )
            }
          }
          setNotice(tRef.current('terminal.error', { message: detail }))
          return
        }
        default:
          // Client-to-host messages never arrive from the host.
          return
      }
    },
    [commitTabs, handleExit, selectTab, sendControl, writeToTerminal],
  )

  const handleMessage = useCallback(
    (event: MessageEvent) => {
      if (typeof event.data === 'string') {
        const message = decodeControl(event.data)
        if (message !== null) handleControl(message)
        return
      }
      const frame = decodeOutputFrame(event.data as ArrayBuffer)
      if (frame === null) return
      const seq = seqRef.current.get(frame.id)
      if (seq === undefined) return
      const applied = applyOutputFrame(seq, frame)
      if (applied === null) return
      seqRef.current.set(frame.id, applied.seq)
      writeToTerminal(frame.id, applied.data)
    },
    [handleControl, writeToTerminal],
  )

  const connect = useCallback(() => {
    disabledRef.current = false
    fatalRef.current = false
    const target = targetRef.current
    const credential =
      target.sessionToken !== undefined && target.sessionToken !== ''
        ? target.sessionToken
        : target.token
    let ws: WebSocket
    try {
      ws = new WebSocket(
        terminalSocketUrl(
          target.host,
          target.port,
          window.location.protocol,
          credential,
        ),
      )
    } catch {
      setNotice(tRef.current('terminal.unreachable'))
      window.setTimeout(() => setConnection('closed'), 0)
      return
    }
    ws.binaryType = 'arraybuffer'
    wsRef.current = ws

    ws.onopen = () => {
      if (wsRef.current !== ws) return
      setConnection('open')
      for (const tab of tabsRef.current) {
        if (tab.id === null || tab.phase !== 'open') continue
        attachingRef.current.push(tab.id)
        sendControl(terminalAttach(tab.id, seqRef.current.get(tab.id) ?? 0))
        const size = sizeRef.current.get(tab.id)
        if (size !== undefined) {
          sendControl(terminalResize(tab.id, size.cols, size.rows))
        }
      }
      if (tabsRef.current.length === 0) {
        openTab()
      } else {
        const pending = pendingOpenRef.current
        if (pending !== null) {
          const tab = tabsRef.current.find((item) => item.key === pending)
          if (tab !== undefined) {
            sendControl(terminalOpen(tab.cols, tab.rows))
          }
        }
      }
    }
    ws.onmessage = (event) => {
      if (wsRef.current === ws) handleMessage(event)
    }
    ws.onclose = () => {
      if (wsRef.current !== ws) return
      wsRef.current = null
      if (fatalRef.current) return
      setConnection('closed')
      // A term-open whose reply never arrived cannot be attached later: its id
      // is unknown. The tab becomes a fresh-open button instead of a ghost.
      pendingOpenRef.current = null
      attachingRef.current = []
      commitTabs(
        tabsRef.current.map((tab) =>
          tab.phase === 'opening' ? { ...tab, phase: 'lost' } : tab,
        ),
      )
    }
    // onclose follows onerror for every browser; keeping onerror empty leaves
    // the state machine in one place.
    ws.onerror = () => undefined
  }, [commitTabs, handleMessage, openTab, sendControl])

  const reconnect = useCallback(() => {
    const old = wsRef.current
    wsRef.current = null
    old?.close()
    setNotice(null)
    setConnection('connecting')
    connect()
  }, [connect])

  /// The user closed the drawer: end every process they can see. Unmounting
  /// for another reason (switching the desktop tab unmounts the whole session
  /// view) only detaches, because the host deliberately keeps terminals alive
  /// when a viewer goes away.
  const closePanel = useCallback(() => {
    for (const tab of tabsRef.current) {
      if (
        tab.id !== null &&
        tab.phase !== 'exited' &&
        tab.phase !== 'lost'
      ) {
        sendControl(terminalClose(tab.id))
      }
    }
    onClose()
  }, [onClose, sendControl])

  useEffect(() => {
    // Deferred one tick: opening the socket synchronously from the effect body
    // would set state during the commit (and StrictMode mounts the effect
    // twice before the first connection could even open).
    const timer = window.setTimeout(() => connect(), 0)
    // These maps live for the whole panel and are only mutated, so the
    // instances captured here are the ones the cleanup must drain.
    const resizers = resizersRef.current
    const writers = writersRef.current
    const queued = queuedRef.current
    const seqs = seqRef.current
    const sizes = sizeRef.current
    return () => {
      window.clearTimeout(timer)
      // Detach only; `closePanel` is what explicitly kills processes. A tab
      // switch or a lost browser connection keeps the host session running so
      // a reconnect could still attach to it.
      const ws = wsRef.current
      wsRef.current = null
      ws?.close()
      for (const resizer of resizers.values()) resizer.cancel()
      resizers.clear()
      writers.clear()
      queued.clear()
      seqs.clear()
      sizes.clear()
      attachingRef.current = []
    }
  }, [connect])

  const activeTab = tabs.find((tab) => tab.key === activeKey)
  const connecting = connection === 'connecting'
  const statusText =
    connection === 'open'
      ? t('terminal.connected')
      : connection === 'connecting'
        ? t('terminal.connecting')
        : connection === 'disabled'
          ? t('terminal.disabledShort')
          : connection === 'fatal'
            ? t('terminal.refused')
            : t('terminal.disconnected')

  return (
    <div
      data-local-input="true"
      className="absolute inset-y-0 right-0 z-40 flex w-[min(96vw,900px)] flex-col border-l border-line bg-surface/95 shadow-2xl backdrop-blur-md"
      onPointerDown={(event) => event.stopPropagation()}
      onPointerUp={(event) => event.stopPropagation()}
    >
      <div className="flex h-11 shrink-0 items-center gap-2 border-b border-line-soft px-3">
        <TerminalIcon size={15} className="text-accent" />
        <span className="text-[13px] font-medium">{t('terminal.title')}</span>
        <span
          className={
            'h-1.5 w-1.5 rounded-full ' +
            (connection === 'open'
              ? 'bg-accent'
              : connecting
                ? 'animate-pulse bg-warn'
                : connection === 'closed'
                  ? 'bg-muted/60'
                  : 'bg-danger')
          }
          aria-hidden="true"
        />
        <span className="text-[10px] text-muted">{statusText}</span>
        <button
          type="button"
          onClick={closePanel}
          className="ml-auto grid h-7 w-7 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink"
          aria-label={t('terminal.close')}
          title={t('terminal.close')}
        >
          <X size={15} />
        </button>
      </div>

      <div className="flex h-9 shrink-0 items-center gap-1 overflow-x-auto border-b border-line-soft px-2">
        {tabs.map((tab) => (
          <div
            key={tab.key}
            className={
              'group flex shrink-0 items-center gap-1.5 rounded-lg pl-3 pr-1.5 text-[12px] transition ' +
              (tab.key === activeKey
                ? 'bg-accent-soft text-accent'
                : 'text-muted hover:bg-panel hover:text-ink')
            }
          >
            <button
              type="button"
              onClick={() => selectTab(tab.key)}
              className="flex items-center gap-1.5 py-1"
            >
              <span
                className={
                  'h-1.5 w-1.5 rounded-full ' +
                  (tab.phase === 'open'
                    ? 'bg-accent'
                    : tab.phase === 'opening'
                      ? 'animate-pulse bg-warn'
                      : tab.phase === 'exited'
                        ? 'bg-muted/60'
                        : 'bg-danger')
                }
                aria-hidden="true"
              />
              {t('terminal.tab', { n: tab.n })}
            </button>
            <button
              type="button"
              onClick={() => closeTab(tab.key)}
              className="grid h-5 w-5 place-items-center rounded text-muted transition hover:bg-panel hover:text-danger"
              aria-label={t('terminal.closeTab')}
              title={t('terminal.closeTab')}
            >
              <X size={12} />
            </button>
          </div>
        ))}
        {connection !== 'disabled' ? (
          <button
            type="button"
            onClick={openTab}
            disabled={
              tabs.length >= MAX_TERMINALS ||
              tabs.some((tab) => tab.phase === 'opening')
            }
            className="ml-1 grid h-6 w-6 shrink-0 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink disabled:cursor-not-allowed disabled:opacity-40"
            aria-label={t('terminal.new')}
            title={
              tabs.length >= MAX_TERMINALS
                ? t('terminal.limit')
                : t('terminal.new')
            }
          >
            <Plus size={14} />
          </button>
        ) : null}
      </div>

      {notice !== null ? (
        <div className="mx-2 mt-2 shrink-0 rounded-lg border border-danger/35 bg-danger/10 px-3 py-2 text-[11px] text-danger">
          {notice}
        </div>
      ) : null}

      {connection === 'closed' ? (
        <div className="mx-2 mt-2 flex shrink-0 items-center gap-2 rounded-lg border border-warn/35 bg-warn/10 px-3 py-2 text-[11px] text-warn">
          <span className="min-w-0 flex-1">{t('terminal.disconnectedHint')}</span>
          <button
            type="button"
            onClick={reconnect}
            className="btn btn-primary h-7 shrink-0 px-2 text-[11px]"
          >
            {t('terminal.reconnect')}
          </button>
        </div>
      ) : null}

      <div className="relative min-h-0 flex-1 overflow-hidden bg-[#0b0e14] p-1">
        {connection === 'disabled' ? (
          <div className="absolute inset-0 grid place-items-center px-6 text-center">
            <div className="max-w-sm text-[12px] text-muted">
              <TerminalIcon size={26} className="mx-auto mb-2 text-muted/70" />
              {t('terminal.disabled')}
            </div>
          </div>
        ) : null}
        {tabs.map((tab) =>
          tab.id === null ? null : (
            <div
              key={tab.key}
              className={
                tab.key === activeKey ? 'absolute inset-1' : 'hidden'
              }
            >
              <TerminalTabView
                terminalId={tab.id}
                active={tab.key === activeKey}
                onInput={handleInput}
                onResize={handleResize}
                registerWriter={registerWriter}
              />
            </div>
          ),
        )}
        {tabs.length === 0 && connection !== 'disabled' ? (
          <div className="absolute inset-0 grid place-items-center px-6 text-center">
            <div className="text-[12px] text-muted">
              {connecting ? t('terminal.connecting') : t('terminal.empty')}
              {connection === 'open' ? (
                <button
                  type="button"
                  onClick={openTab}
                  className="btn btn-primary mt-3 block h-8 px-3 text-[12px]"
                >
                  {t('terminal.new')}
                </button>
              ) : null}
            </div>
          </div>
        ) : null}
        {activeTab !== undefined &&
        activeTab.id === null &&
        activeTab.phase === 'opening' ? (
          <div className="pointer-events-none absolute inset-x-0 bottom-2 text-center text-[11px] text-muted">
            {t('terminal.opening')}
          </div>
        ) : null}
      </div>
    </div>
  )
}
