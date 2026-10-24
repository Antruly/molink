import { MessageSquare, Phone, PhoneOff, Send, X } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import {
  fetchCall,
  fetchChat,
  postCall,
  postChat,
  type CallState,
  type ChatEntry,
} from '../lib/api'
import { useAppStore } from '../store/app'

function timeLabel(ts: number): string {
  if (!ts) return ''
  try {
    return new Date(ts).toLocaleTimeString(undefined, {
      hour: '2-digit',
      minute: '2-digit',
    })
  } catch {
    return ''
  }
}

/// Chat dock for the person sitting at this machine. A controlling client
/// talks over the session websocket; the host talks through /api/chat, and
/// both land in the same core-side log.
export function HostChatDock() {
  const { t } = useTranslation()
  const localMode = useAppStore((state) => state.localMode)
  const token = useAppStore((state) => state.token)
  const immersive = useAppStore((state) => state.immersive)

  const [open, setOpen] = useState(false)
  const [messages, setMessages] = useState<ChatEntry[]>([])
  const [unread, setUnread] = useState(0)
  const [text, setText] = useState('')
  const [call, setCall] = useState<CallState | null>(null)
  const [callBusy, setCallBusy] = useState(false)
  const [callSeconds, setCallSeconds] = useState(0)
  const cursorRef = useRef(0)
  const openRef = useRef(false)
  const listRef = useRef<HTMLDivElement>(null)
  const pollRef = useRef<() => Promise<void>>(async () => {})

  useEffect(() => {
    openRef.current = open
  }, [open])

  useEffect(() => {
    if (!localMode || !token) return
    let alive = true
    const poll = async () => {
      try {
        const result = await fetchChat(token, cursorRef.current)
        if (!alive) return
        cursorRef.current = result.cursor
        if (result.messages.length > 0) {
          setMessages((previous) =>
            [...previous, ...result.messages].slice(-200),
          )
          if (!openRef.current) {
            const incoming = result.messages.filter(
              (message) => message.from !== 'host',
            ).length
            if (incoming > 0) setUnread((count) => count + incoming)
          }
        }
      } catch {
        // The core may be restarting; the next poll picks it up.
      }
      try {
        const state = await fetchCall(token)
        if (alive) setCall(state)
      } catch {
        // Voice calls may be unavailable on this host.
      }
    }
    pollRef.current = poll
    void poll()
    const timer = window.setInterval(() => void poll(), 2000)
    return () => {
      alive = false
      window.clearInterval(timer)
    }
  }, [localMode, token])

  useEffect(() => {
    if (!open) return
    const element = listRef.current
    if (element) element.scrollTop = element.scrollHeight
  }, [messages, open])

  useEffect(() => {
    if (call?.state !== 'active' || !call.started_ms) return
    const timer = window.setInterval(() => {
      setCallSeconds(Math.floor((Date.now() - call.started_ms) / 1000))
    }, 1000)
    return () => window.clearInterval(timer)
  }, [call?.state, call?.started_ms])

  if (!localMode || immersive) return null

  const callAction = async (action: 'accept' | 'decline' | 'end') => {
    if (!token) return
    setCallBusy(true)
    try {
      await postCall(token, action)
      const state = await fetchCall(token)
      setCall(state)
      if (state.state === 'active') setCallSeconds(0)
    } catch {
      // The call may have ended on the other side already.
    } finally {
      setCallBusy(false)
    }
  }

  const formatCallTime = (seconds: number) => {
    const minutes = Math.floor(seconds / 60)
    return `${minutes}:${String(seconds % 60).padStart(2, '0')}`
  }

  const send = async () => {
    const value = text.trim()
    if (value === '' || !token) return
    setText('')
    try {
      await postChat(token, value)
      await pollRef.current()
    } catch {
      // Keep the draft when the core is unavailable.
      setText(value)
    }
  }

  return (
    <>
      {call && call.state !== 'idle' ? (
        <div className="fixed right-4 top-20 z-40 w-[min(92vw,340px)] rounded-2xl border border-accent/40 bg-surface/95 p-3 shadow-2xl backdrop-blur">
          <div className="flex items-center gap-2">
            <span className="grid h-8 w-8 place-items-center rounded-full bg-accent-soft text-accent">
              <Phone size={15} />
            </span>
            <div className="min-w-0 flex-1">
              <div className="text-[12px] font-medium text-ink">
                {call.state === 'ringing'
                  ? t('call.incoming')
                  : t('call.inCall')}
              </div>
              <div className="text-[10px] text-muted">
                {call.state === 'active'
                  ? formatCallTime(callSeconds)
                  : call.host_microphone
                    ? t('call.hostMicReady')
                    : t('call.hostNoMic')}
              </div>
            </div>
          </div>
          {call.state === 'ringing' ? (
            <div className="mt-2.5 flex gap-1.5">
              <button
                type="button"
                disabled={callBusy}
                onClick={() => void callAction('accept')}
                className="btn btn-primary h-8 flex-1 gap-1 text-[11px]"
              >
                <Phone size={13} />
                {t('call.accept')}
              </button>
              <button
                type="button"
                disabled={callBusy}
                onClick={() => void callAction('decline')}
                className="btn h-8 flex-1 gap-1 text-[11px]"
              >
                <PhoneOff size={13} />
                {t('call.decline')}
              </button>
            </div>
          ) : (
            <div className="mt-2.5 flex gap-1.5">
              <span className="flex flex-1 items-center gap-1 text-[10px] text-accent">
                <Phone size={12} />
                {t('call.activeHint')}
              </span>
              <button
                type="button"
                disabled={callBusy}
                onClick={() => void callAction('end')}
                className="btn h-8 gap-1 border-danger/40 px-2 text-[11px] text-danger"
              >
                <PhoneOff size={13} />
                {t('call.end')}
              </button>
            </div>
          )}
        </div>
      ) : null}
      {open ? (
        <div className="fixed bottom-4 right-4 z-30 flex h-[440px] w-[min(92vw,340px)] flex-col overflow-hidden rounded-2xl border border-line bg-surface/95 shadow-2xl backdrop-blur">
          <div className="flex h-10 shrink-0 items-center gap-2 border-b border-line-soft px-3">
            <MessageSquare size={14} className="text-accent" />
            <span className="text-[12px] font-medium">{t('chat.title')}</span>
            <span className="chip ml-1 !px-1.5 !py-0 text-[9px]">
              {t('chat.host')}
            </span>
            <button
              type="button"
              onClick={() => setOpen(false)}
              className="ml-auto grid h-6 w-6 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink"
              aria-label={t('chat.close')}
              title={t('chat.close')}
            >
              <X size={14} />
            </button>
          </div>
          <div
            ref={listRef}
            className="min-h-0 flex-1 space-y-2 overflow-y-auto px-3 py-3"
          >
            {messages.length === 0 ? (
              <div className="rounded-xl border border-dashed border-line px-3 py-8 text-center text-[11px] leading-relaxed text-muted">
                {t('chat.emptyHost')}
              </div>
            ) : (
              messages.map((message) => {
                const mine = message.from === 'host'
                return (
                  <div
                    key={`${message.id}-${message.ts}`}
                    className={mine ? 'flex justify-end' : 'flex justify-start'}
                  >
                    <div
                      className={
                        'max-w-[85%] rounded-2xl px-3 py-2 text-[12px] leading-relaxed ' +
                        (mine
                          ? 'rounded-br-md bg-accent text-on-accent'
                          : 'rounded-bl-md border border-line bg-panel text-ink')
                      }
                    >
                      <div className="whitespace-pre-wrap break-words">
                        {message.text}
                      </div>
                      <div
                        className={
                          'mt-1 text-right text-[9px] ' +
                          (mine ? 'text-on-accent/60' : 'text-muted/70')
                        }
                      >
                        {mine ? t('chat.host') : t('chat.remote')} ·{' '}
                        {timeLabel(message.ts)}
                      </div>
                    </div>
                  </div>
                )
              })
            )}
          </div>
          <div className="flex shrink-0 items-end gap-2 border-t border-line-soft p-2.5">
            <textarea
              value={text}
              onChange={(event) => setText(event.target.value)}
              onKeyDown={(event) => {
                if (event.key === 'Enter' && !event.shiftKey) {
                  event.preventDefault()
                  void send()
                }
              }}
              rows={2}
              maxLength={4096}
              placeholder={t('chat.placeholder')}
              className="field min-h-9 resize-none !py-2 text-[12px]"
            />
            <button
              type="button"
              onClick={() => void send()}
              disabled={text.trim() === ''}
              className="btn btn-primary h-9 w-9 shrink-0 !px-0"
              aria-label={t('chat.send')}
              title={t('chat.send')}
            >
              <Send size={14} />
            </button>
          </div>
        </div>
      ) : (
        <button
          type="button"
          onClick={() => {
            setUnread(0)
            setOpen(true)
          }}
          className="fixed bottom-4 right-4 z-30 grid h-12 w-12 place-items-center rounded-full border border-line bg-surface text-accent shadow-xl transition hover:border-accent/50 hover:bg-panel"
          title={t('chat.title')}
          aria-label={t('chat.title')}
        >
          <MessageSquare size={18} />
          {unread > 0 ? (
            <span className="absolute -right-0.5 -top-0.5 grid h-5 min-w-5 place-items-center rounded-full bg-accent px-1 text-[10px] font-semibold text-on-accent">
              {unread > 9 ? '9+' : unread}
            </span>
          ) : null}
        </button>
      )}
    </>
  )
}
