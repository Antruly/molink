import { GripHorizontal, MessageSquare, Phone, PhoneOff, X } from 'lucide-react'
import { useCallback, useEffect, useMemo, useRef, useState, type PointerEvent as ReactPointerEvent } from 'react'
import { useTranslation } from 'react-i18next'
import {
  fetchCall,
  fetchChat,
  postCall,
  postChat,
  type CallState,
  type ChatEntry,
} from '../lib/api'
import {
  attachmentKind,
  chatDisplayName,
  encodeChatPayload,
  fitsChatText,
  hostChatDirectory,
  joinHostPath,
  localDateString,
  MAX_CHAT_ATTACHMENT_BYTES,
  mimeForName,
  newAttachmentId,
  parseChatPayload,
  safeChatFileName,
  type ChatAttachment,
} from '../lib/chat'
import { HostAttachmentAccess } from '../lib/chat-access'
import {
  attachmentThumbnail,
  dataUrlToBlob,
  type ChatThumbnail,
} from '../lib/chat-media'
import { writeHostFile } from '../lib/chat-transfer'
import { useAppStore } from '../store/app'
import { ChatComposer } from './chat/ChatComposer'
import { ChatFeed } from './chat/ChatFeed'
import type { ChatDelivery, ChatFeedRow } from './chat/ChatBubble'
import { useFileDrop } from './chat/chat-hooks'

interface HostPending {
  localId: string
  text: string
  attachments: ChatAttachment[]
  ts: number
  status: ChatDelivery
  progress?: number
  retry: () => void
}

const CALL_TIME = (seconds: number) => {
  const minutes = Math.floor(seconds / 60)
  return `${minutes}:${String(seconds % 60).padStart(2, '0')}`
}

/// Chat dock for the person sitting at this machine. A controlling client
/// talks over the session websocket; the host talks through /api/chat, and
/// both land in the same core-side log.
export function HostChatDock() {
  const { t } = useTranslation()
  const localMode = useAppStore((state) => state.localMode)
  const token = useAppStore((state) => state.token)
  const immersive = useAppStore((state) => state.immersive)
  const security = useAppStore((state) => state.security)
  const device = useAppStore((state) => state.device)

  const [open, setOpen] = useState(false)
  const [messages, setMessages] = useState<ChatEntry[]>([])
  const [pending, setPending] = useState<HostPending[]>([])
  const [hint, setHint] = useState<string | null>(null)
  const [unread, setUnread] = useState(0)
  const [call, setCall] = useState<CallState | null>(null)
  const [callBusy, setCallBusy] = useState(false)
  const [callSeconds, setCallSeconds] = useState(0)
  const cursorRef = useRef(0)
  const openRef = useRef(false)
  const hintTimer = useRef(0)
  const seen = useRef(new Set<string>())
  const writeChain = useRef<Promise<unknown>>(Promise.resolve())
  // A pending bubble outlives a re-login; its retry always uses the current
  // token and attachment writer.
  const tokenRef = useRef(token)
  useEffect(() => {
    tokenRef.current = token
  }, [token])
  // The dock is a floating card: both the collapsed bubble and the open card
  // can be dragged anywhere in the window.
  const [pos, setPos] = useState<{ x: number; y: number } | null>(null)
  const panelRef = useRef<HTMLDivElement>(null)
  const ballRef = useRef<HTMLButtonElement>(null)
  const drag = useRef<{
    offsetX: number
    offsetY: number
    startX: number
    startY: number
    moved: boolean
    target: HTMLElement
  } | null>(null)
  const suppressClick = useRef(false)
  const ringingRef = useRef(false)

  const downloadRoot = security?.download_root || security?.download_root_default || ''
  const deviceId = device?.device_id || 'self'
  const canAttach = Boolean(token && downloadRoot !== '')

  const access = useMemo(
    () => new HostAttachmentAccess(token, downloadRoot),
    [token, downloadRoot],
  )
  useEffect(() => {
    access.revive()
    return () => access.dispose()
  }, [access])

  const showHint = useCallback((message: string) => {
    setHint(message)
    window.clearTimeout(hintTimer.current)
    hintTimer.current = window.setTimeout(() => setHint(null), 6000)
  }, [])

  const patchPending = useCallback(
    (localId: string, patch: Partial<HostPending>) => {
      setPending((list) =>
        list.map((item) =>
          item.localId === localId ? { ...item, ...patch } : item,
        ),
      )
    },
    [],
  )

  const enqueue = useCallback(<T,>(task: () => Promise<T>): Promise<T> => {
    const next = writeChain.current.then(task, task)
    writeChain.current = next.then(
      () => undefined,
      () => undefined,
    )
    return next
  }, [])

  useEffect(() => {
    openRef.current = open
  }, [open])

  // Keep the card inside the window when it opens from a dragged bubble (or
  // the window shrinks).
  useEffect(() => {
    if (!open || !pos) return
    const element = panelRef.current
    if (!element) return
    const maxX = Math.max(4, window.innerWidth - element.offsetWidth - 4)
    const maxY = Math.max(4, window.innerHeight - element.offsetHeight - 4)
    if (pos.x > maxX || pos.y > maxY) {
      setPos({ x: Math.min(pos.x, maxX), y: Math.min(pos.y, maxY) })
    }
  }, [open, pos])

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
            [...previous, ...result.messages].slice(-500),
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
    void poll()
    const timer = window.setInterval(() => void poll(), 2000)
    return () => {
      alive = false
      window.clearInterval(timer)
    }
  }, [localMode, token])

  useEffect(() => {
    if (call?.state !== 'active' || !call.started_ms) return
    const timer = window.setInterval(() => {
      setCallSeconds(Math.floor((Date.now() - call.started_ms) / 1000))
    }, 1000)
    return () => window.clearInterval(timer)
  }, [call?.state, call?.started_ms])

  // A ringing call must reach the person at this machine even when they are
  // in an immersive session or another app has focus. Flash the title and use
  // a system notification when the browser allows it; the in-page card below
  // is always shown as the fallback.
  useEffect(() => {
    const ringing = call?.state === 'ringing'
    if (ringing === ringingRef.current) return
    ringingRef.current = ringing
    if (!ringing) return
    const original = document.title
    let flip = false
    const timer = window.setInterval(() => {
      document.title = flip
        ? original
        : `📞 ${t('call.incoming')} · moLink`
      flip = !flip
    }, 900)
    if ('Notification' in window) {
      try {
        if (Notification.permission === 'granted') {
          const notification = new Notification(t('call.incoming'), {
            body: t('call.incomingBody'),
            tag: 'molink-incoming-call',
          })
          notification.onclick = () => {
            window.focus()
            notification.close()
          }
        } else if (Notification.permission === 'default') {
          void Notification.requestPermission()
        }
      } catch {
        // Notifications are optional; the card and title still work.
      }
    }
    return () => {
      window.clearInterval(timer)
      document.title = original
    }
  }, [call?.state, t])

  const startAttachment = useCallback(
    (
      file: File,
      id: string,
      localId: string,
      ts: number,
      kind: ChatAttachment['kind'],
    ) => {
      patchPending(localId, { status: 'sending', progress: 0, ts })
      void enqueue(async () => {
        let media: ChatThumbnail | null = null
        const mime = file.type || mimeForName(file.name)
        const displayName = chatDisplayName(file.name, mime, id)
        const base: ChatAttachment = {
          id,
          name: displayName,
          size: file.size,
          mime,
          kind,
          thumb: '',
          src: '',
          w: 0,
          h: 0,
          dur: 0,
        }
        try {
          if (!token) throw new Error('offline')
          const directory = hostChatDirectory(
            downloadRoot,
            deviceId,
            localDateString(),
          )
          if (directory === '') throw new Error('no_storage')
          const source = joinHostPath(
            directory,
            `${id}-${safeChatFileName(displayName)}`,
          )
          if (kind === 'image' || kind === 'video') {
            media = await attachmentThumbnail(file, file.name, kind)
            if (media) {
              const preview = media.dataUrl
              access.rememberLocal(id, file, preview === '' ? undefined : preview)
              patchPending(localId, {
                attachments: [
                  {
                    ...base,
                    thumb: preview,
                    w: media.w,
                    h: media.h,
                    dur: media.dur,
                  },
                ],
              })
            } else {
              access.rememberLocal(id, file)
            }
          } else {
            access.rememberLocal(id, file)
          }
          await writeHostFile({
            token,
            path: source,
            blob: file,
            onProgress: (sent, total) => {
              patchPending(localId, {
                progress: total > 0 ? sent / total : 1,
              })
            },
          })
          let thumbPath = ''
          if (media && media.dataUrl !== '') {
            thumbPath = joinHostPath(directory, `thumb-${id}.webp`)
            try {
              await writeHostFile({
                token,
                path: thumbPath,
                blob: dataUrlToBlob(media.dataUrl),
              })
            } catch {
              thumbPath = ''
            }
          }
          const attachment: ChatAttachment = {
            ...base,
            thumb: thumbPath,
            src: source,
            w: media?.w ?? 0,
            h: media?.h ?? 0,
            dur: media?.dur ?? 0,
          }
          patchPending(localId, { attachments: [attachment], progress: 1 })
          const encoded = encodeChatPayload({
            type: 'chat',
            text: '',
            attachments: [attachment],
          })
          if (!fitsChatText(encoded)) throw new Error('message_too_long')
          await postChat(token, encoded)
          patchPending(localId, { status: 'sent', progress: undefined })
        } catch (error) {
          patchPending(localId, { status: 'failed', progress: undefined })
          const reason = error instanceof Error ? error.message : ''
          if (reason === 'message_too_long') {
            showHint(t('chat.attachmentNameTooLong'))
          } else if (reason === 'no_storage') {
            showHint(t('chat.noStorage'))
          } else {
            showHint(t('chat.uploadFailed'))
          }
        }
      })
    },
    [access, deviceId, downloadRoot, enqueue, patchPending, showHint, t, token],
  )

  const startAttachmentRef = useRef(startAttachment)
  useEffect(() => {
    startAttachmentRef.current = startAttachment
  })

  const sendFiles = useCallback(
    (files: File[]) => {
      if (files.length === 0) return
      if (!token) {
        showHint(t('chat.offline'))
        return
      }
      if (!canAttach) {
        showHint(t('chat.noStorage'))
        return
      }
      for (const file of files) {
        if (file.size > MAX_CHAT_ATTACHMENT_BYTES) {
          showHint(
            t('chat.attachmentTooLarge', {
              name: file.name,
              limit: '200 MiB',
            }),
          )
          continue
        }
        const id = newAttachmentId()
        const ts = Date.now()
        const kind = attachmentKind(file.name, file.type)
        const mime = file.type || mimeForName(file.name)
        setPending((list) => [
          ...list,
          {
            localId: id,
            text: '',
            attachments: [
              {
                id,
                name: chatDisplayName(file.name, mime, id),
                size: file.size,
                mime,
                kind,
                thumb: '',
                src: '',
                w: 0,
                h: 0,
                dur: 0,
              },
            ],
            ts,
            status: 'sending',
            progress: 0,
            retry: () =>
              startAttachmentRef.current(file, id, id, Date.now(), kind),
          },
        ])
        startAttachment(file, id, id, ts, kind)
      }
    },
    [canAttach, showHint, startAttachment, t, token],
  )

  const retryText = useCallback(
    (localId: string, text: string) => {
      const current = tokenRef.current
      if (!current) {
        showHint(t('chat.offline'))
        return
      }
      patchPending(localId, { status: 'sending' })
      void postChat(current, text)
        .then(() => patchPending(localId, { status: 'sent' }))
        .catch(() => {
          patchPending(localId, { status: 'failed' })
          showHint(t('chat.sendFailed'))
        })
    },
    [patchPending, showHint, t],
  )

  const sendText = (value: string): boolean => {
    if (!token) return false
    if (!fitsChatText(value)) {
      showHint(t('chat.messageTooLong'))
      return false
    }
    const localId = newAttachmentId()
    const ts = Date.now()
    setPending((list) => [
      ...list,
      {
        localId,
        text: value,
        attachments: [],
        ts,
        status: 'sending',
        retry: () => retryText(localId, value),
      },
    ])
    void postChat(token, value)
      .then(() => patchPending(localId, { status: 'sent' }))
      .catch(() => {
        patchPending(localId, { status: 'failed' })
        showHint(t('chat.sendFailed'))
      })
    return true
  }

  // The polled log is authoritative: drop the optimistic row once its copy
  // arrives (the host usually sees it within one poll interval).
  useEffect(() => {
    if (pending.length === 0) return
    setPending((current) => {
      if (current.length === 0) return current
      const remaining = [...current]
      for (const message of messages) {
        if (message.from !== 'host') continue
        const key = `m${message.id}`
        if (seen.current.has(key)) continue
        const payload = parseChatPayload(message.text)
        if (payload && payload.attachments.length > 0) {
          const ids = new Set(payload.attachments.map((item) => item.id))
          const index = remaining.findIndex((item) =>
            item.attachments.some((attachment) => ids.has(attachment.id)),
          )
          if (index >= 0) {
            remaining.splice(index, 1)
            seen.current.add(key)
          }
          continue
        }
        const text = payload?.text ?? message.text
        const index = remaining.findIndex(
          (item) =>
            item.attachments.length === 0 &&
            item.text === text &&
            Math.abs(item.ts - message.ts) < 120_000,
        )
        if (index >= 0) {
          remaining.splice(index, 1)
          seen.current.add(key)
        }
      }
      return remaining.length === current.length ? current : remaining
    })
  }, [messages, pending.length])

  const selfName = device?.device_name?.trim() || t('chat.host')

  const rows: ChatFeedRow[] = []
  const echoedIds = new Set<string>()
  for (const message of messages) {
    const payload = parseChatPayload(message.text)
    if (payload) {
      for (const attachment of payload.attachments) echoedIds.add(attachment.id)
    }
    const mine = message.from === 'host'
    rows.push({
      key: `srv-${message.id}-${message.ts}`,
      mine,
      senderName:
        message.sender_name?.trim() ||
        (mine ? selfName : t('chat.remote')),
      ...(message.sender_id ? { senderId: message.sender_id } : {}),
      text: payload ? payload.text : message.text,
      attachments: payload?.attachments ?? [],
      ts: message.ts,
      ...(mine ? { status: 'sent' as const } : {}),
    })
  }
  for (const item of pending) {
    if (item.attachments.some((attachment) => echoedIds.has(attachment.id))) {
      continue
    }
    rows.push({
      key: `local-${item.localId}`,
      mine: true,
      senderName: selfName,
      text: item.text,
      attachments: item.attachments,
      ts: item.ts,
      status: item.status,
      ...(item.progress !== undefined ? { progress: item.progress } : {}),
      onRetry: item.retry,
    })
  }

  const drop = useFileDrop(Boolean(token && canAttach), sendFiles)

  if (!localMode) return null

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

  const beginDrag = (
    event: ReactPointerEvent<HTMLElement>,
    target: HTMLElement | null,
  ) => {
    if (!target) return
    event.preventDefault()
    event.stopPropagation()
    const rect = target.getBoundingClientRect()
    drag.current = {
      offsetX: event.clientX - rect.left,
      offsetY: event.clientY - rect.top,
      startX: event.clientX,
      startY: event.clientY,
      moved: false,
      target,
    }
    setPos({ x: rect.left, y: rect.top })
    event.currentTarget.setPointerCapture(event.pointerId)
  }

  const moveDrag = (event: ReactPointerEvent<HTMLElement>) => {
    const state = drag.current
    if (!state) return
    if (
      !state.moved &&
      Math.hypot(event.clientX - state.startX, event.clientY - state.startY) > 4
    ) {
      state.moved = true
    }
    const width = state.target.offsetWidth
    const height = state.target.offsetHeight
    setPos({
      x: Math.min(
        Math.max(event.clientX - state.offsetX, 4),
        Math.max(4, window.innerWidth - width - 4),
      ),
      y: Math.min(
        Math.max(event.clientY - state.offsetY, 4),
        Math.max(4, window.innerHeight - height - 4),
      ),
    })
  }

  const endDrag = () => {
    const state = drag.current
    drag.current = null
    if (!state?.moved) return
    suppressClick.current = true
    window.setTimeout(() => {
      suppressClick.current = false
    }, 0)
  }

  return (
    <>
      {call && call.state !== 'idle' ? (
        <div
          data-local-input="true"
          className={
            'fixed z-50 w-[min(92vw,340px)] rounded-2xl border bg-surface/95 p-3 shadow-2xl backdrop-blur ' +
            (immersive
              ? 'left-1/2 top-4 -translate-x-1/2 border-accent/60 ring-4 ring-accent/20'
              : 'right-4 top-20 border-accent/40')
          }
        >
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
                  ? CALL_TIME(callSeconds)
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
        <div
          ref={panelRef}
          data-local-input="true"
          className={
            'fixed z-50 flex h-[440px] w-[min(92vw,340px)] flex-col overflow-hidden rounded-2xl border border-line bg-surface/95 shadow-2xl backdrop-blur ' +
            (pos ? '' : 'bottom-4 right-4')
          }
          style={pos ? { left: pos.x, top: pos.y } : undefined}
          {...drop.handlers}
        >
          <div
            className="flex h-10 shrink-0 cursor-move touch-none items-center gap-2 border-b border-line-soft px-3"
            onPointerDown={(event) => beginDrag(event, panelRef.current)}
            onPointerMove={moveDrag}
            onPointerUp={endDrag}
            onPointerCancel={endDrag}
          >
            <GripHorizontal size={13} className="text-muted/60" />
            <MessageSquare size={14} className="text-accent" />
            <span className="text-[12px] font-medium">{t('chat.title')}</span>
            <span className="chip ml-1 !px-1.5 !py-0 text-[9px]">
              {t('chat.host')}
            </span>
            <button
              type="button"
              onPointerDown={(event) => event.stopPropagation()}
              onClick={() => setOpen(false)}
              className="ml-auto grid h-6 w-6 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink"
              aria-label={t('chat.close')}
              title={t('chat.close')}
            >
              <X size={14} />
            </button>
          </div>
          <ChatFeed
            rows={rows}
            access={access}
            selfName={selfName}
            empty={t('chat.emptyHost')}
            conversationKey="host"
          />
          <ChatComposer
            onSendText={sendText}
            onSendFiles={sendFiles}
            attachEnabled={canAttach}
            placeholder={t('chat.placeholder')}
            hint={hint}
          />
          {drop.dragging ? (
            <div className="pointer-events-none absolute inset-0 z-50 grid place-items-center border-2 border-dashed border-accent bg-surface/85 text-[12px] font-medium text-accent">
              {t('chat.dropHint')}
            </div>
          ) : null}
        </div>
      ) : (
        <button
          ref={ballRef}
          type="button"
          data-local-input="true"
          onPointerDown={(event) => beginDrag(event, ballRef.current)}
          onPointerMove={moveDrag}
          onPointerUp={endDrag}
          onPointerCancel={endDrag}
          onClick={() => {
            if (suppressClick.current) {
              suppressClick.current = false
              return
            }
            setUnread(0)
            setOpen(true)
          }}
          className={
            'fixed z-50 grid h-12 w-12 cursor-move touch-none place-items-center rounded-full border border-line bg-surface text-accent shadow-xl transition hover:border-accent/50 hover:bg-panel ' +
            (pos ? '' : 'bottom-4 right-4')
          }
          style={pos ? { left: pos.x, top: pos.y } : undefined}
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
