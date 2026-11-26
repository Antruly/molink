import { ExternalLink, MessageSquare, Phone, X } from 'lucide-react'
import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import type { ChatMessage } from '../lib/client'
import {
  attachmentKind,
  buildOutgoingMessage,
  chatDisplayName,
  chatThumbName,
  chatUploadName,
  fitsChatText,
  MAX_CHAT_ATTACHMENT_BYTES,
  mimeForName,
  newAttachmentId,
  parseChatPayload,
  type ChatAttachment,
} from '../lib/chat'
import { RemoteAttachmentAccess } from '../lib/chat-access'
import {
  dataUrlToBlob,
  attachmentThumbnail,
  type ChatThumbnail,
} from '../lib/chat-media'
import { uploadChatFile } from '../lib/chat-transfer'
import type { SessionConnection } from '../lib/session-connection'
import { ChatComposer } from './chat/ChatComposer'
import { ChatFeed } from './chat/ChatFeed'
import type { ChatDelivery, ChatFeedRow } from './chat/ChatBubble'
import { useFileDrop } from './chat/chat-hooks'

interface PendingMessage {
  localId: string
  text: string
  attachments: ChatAttachment[]
  ts: number
  status: ChatDelivery
  progress?: number
  retry: () => void
}

/// Chat drawer shared by the local UI and a controlling client. The core keeps
/// one log per machine, so a message typed on either side appears on both.
export function ChatPanel({
  open,
  onClose,
  messages,
  onSend,
  remoteLabel,
  remoteId,
  selfSessionId,
  selfName,
  connection,
  callAvailable = false,
  callPhase = 'idle',
  onCall,
  fullPage = false,
  onPopOut,
}: {
  open: boolean
  onClose: () => void
  /// Standalone tab: fill the page instead of sliding in from the right.
  fullPage?: boolean
  /// Embedded drawer only: move this pane into its own tab.
  onPopOut?: () => void
  messages: ChatMessage[]
  /// Return false when the session is not live; the pending row then shows the
  /// failed state and can be retried from the bubble.
  onSend: (text: string) => boolean
  remoteLabel: string
  /// Device ID of the peer (shown next to its name).
  remoteId?: string
  /// This viewer's host-side session id, used to mark its own messages when
  /// several people share one chat.
  selfSessionId?: number
  selfName?: string
  /// The session file channel; attachments are uploaded through it.
  connection?: SessionConnection | null
  callAvailable?: boolean
  callPhase?: 'idle' | 'ringing' | 'active'
  onCall?: () => void
}) {
  const { t } = useTranslation()
  const [pending, setPending] = useState<PendingMessage[]>([])
  const [hint, setHint] = useState<string | null>(null)
  const hintTimer = useRef(0)
  const abortRef = useRef(new AbortController())
  const seen = useRef(new Set<string>())
  const uploadChain = useRef<Promise<unknown>>(Promise.resolve())
  // Pending rows outlive a reconnect (and its new SessionConnection / send
  // closure). Their retry callbacks go through these refs so they always use
  // the live channel instead of the one captured when the bubble was created.
  const onSendRef = useRef(onSend)
  useEffect(() => {
    onSendRef.current = onSend
  }, [onSend])

  const access = useMemo(() => new RemoteAttachmentAccess(null), [])
  useEffect(() => {
    access.setConnection(connection ?? null)
  }, [access, connection])
  useEffect(() => {
    access.revive()
    const controller = new AbortController()
    abortRef.current = controller
    return () => {
      access.dispose()
      controller.abort()
    }
  }, [access])

  const showHint = useCallback((message: string) => {
    setHint(message)
    window.clearTimeout(hintTimer.current)
    hintTimer.current = window.setTimeout(() => setHint(null), 6000)
  }, [])

  const patchPending = useCallback(
    (localId: string, patch: Partial<PendingMessage>) => {
      setPending((list) =>
        list.map((item) =>
          item.localId === localId ? { ...item, ...patch } : item,
        ),
      )
    },
    [],
  )

  /// The file channel answers one offer at a time; a second offer would race
  /// the first `accept`, so every attachment task joins one queue.
  const enqueue = useCallback(<T,>(task: () => Promise<T>): Promise<T> => {
    const next = uploadChain.current.then(task, task)
    uploadChain.current = next.then(
      () => undefined,
      () => undefined,
    )
    return next
  }, [])

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
          if (kind === 'image' || kind === 'video') {
            media = await attachmentThumbnail(file, file.name, kind)
            if (media) {
              access.rememberLocal(
                id,
                file,
                media.dataUrl === '' ? undefined : media.dataUrl,
              )
              patchPending(localId, {
                attachments: [
                  {
                    ...base,
                    thumb: media.dataUrl,
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

          if (!connection) throw new Error('offline')
          const uploaded = await uploadChatFile({
            connection,
            name: chatUploadName(displayName),
            blob: file,
            signal: abortRef.current.signal,
            onProgress: (progress) => {
              patchPending(localId, {
                progress: progress.total > 0
                  ? progress.sent / progress.total
                  : 1,
              })
            },
          })

          let thumbPath = ''
          if (media && media.dataUrl !== '') {
            try {
              const thumb = await uploadChatFile({
                connection,
                name: chatThumbName(id),
                blob: dataUrlToBlob(media.dataUrl),
                signal: abortRef.current.signal,
              })
              thumbPath = thumb.path
            } catch {
              // The preview is cosmetic; the message still delivers.
            }
          }

          const attachment: ChatAttachment = {
            ...base,
            thumb: thumbPath,
            src: uploaded.path,
            w: media?.w ?? 0,
            h: media?.h ?? 0,
            dur: media?.dur ?? 0,
          }
          // The wire copy names the uploaded file; the local copy keeps the
          // generated data URI so this client's own bubble paints the preview
          // without a round trip.
          const built = buildOutgoingMessage({
            attachment,
            thumb: media?.dataUrl ?? '',
          })
          patchPending(localId, {
            attachments: built.local.attachments,
            progress: 1,
          })
          if (!fitsChatText(built.text)) throw new Error('message_too_long')
          if (!onSend(built.text)) throw new Error('offline')
          patchPending(localId, { status: 'sent', progress: undefined })
        } catch (error) {
          patchPending(localId, { status: 'failed', progress: undefined })
          const reason = error instanceof Error ? error.message : ''
          showHint(
            reason === 'message_too_long'
              ? t('chat.attachmentNameTooLong')
              : t('chat.uploadFailed'),
          )
        }
      })
    },
    [access, connection, enqueue, onSend, patchPending, showHint, t],
  )

  const startAttachmentRef = useRef(startAttachment)
  useEffect(() => {
    startAttachmentRef.current = startAttachment
  })

  const sendFiles = useCallback(
    (files: File[]) => {
      if (files.length === 0) return
      if (!connection) {
        showHint(t('chat.offline'))
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
        const row: PendingMessage = {
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
        }
        setPending((list) => [...list, row])
        startAttachment(file, id, id, ts, kind)
      }
    },
    [connection, showHint, startAttachment, t],
  )

  const retryText = useCallback(
    (localId: string, text: string) => {
      const ok = onSendRef.current(text)
      patchPending(localId, { status: ok ? 'sent' : 'failed' })
      if (!ok) showHint(t('chat.offline'))
    },
    [patchPending, showHint, t],
  )

  const sendText = (value: string): boolean => {
    if (!fitsChatText(value)) {
      showHint(t('chat.messageTooLong'))
      return false
    }
    const localId = newAttachmentId()
    const ts = Date.now()
    const ok = onSend(value)
    setPending((list) => [
      ...list,
      {
        localId,
        text: value,
        attachments: [],
        ts,
        status: ok ? 'sent' : 'failed',
        retry: () => retryText(localId, value),
      },
    ])
    if (!ok) showHint(t('chat.offline'))
    return true
  }

  const isMine = useCallback(
    (message: ChatMessage) =>
      message.from !== 'host' &&
      (message.senderSession === undefined ||
        selfSessionId === undefined ||
        message.senderSession === selfSessionId),
    [selfSessionId],
  )

  // Drop the optimistic copy once the core echoes the message back. The core
  // broadcasts to every viewer, including the one that sent it.
  useEffect(() => {
    if (pending.length === 0) return
    setPending((current) => {
      if (current.length === 0) return current
      const remaining = [...current]
      for (const message of messages) {
        if (!isMine(message)) continue
        const key = `${message.id}:${message.ts}`
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
  }, [isMine, messages, pending.length])

  const ownName =
    messages.find((message) => isMine(message) && message.senderName?.trim())
      ?.senderName?.trim() ||
    selfName ||
    t('chat.me')

  const rows: ChatFeedRow[] = []
  const echoedIds = new Set<string>()
  for (const message of messages) {
    const payload = parseChatPayload(message.text)
    if (payload) {
      for (const attachment of payload.attachments) echoedIds.add(attachment.id)
    }
    const mine = isMine(message)
    rows.push({
      key: `srv-${message.id}-${message.ts}`,
      mine,
      senderName:
        message.senderName?.trim() ||
        (message.from === 'host' ? t('chat.host') : t('chat.remote')),
      ...(message.senderId ? { senderId: message.senderId } : {}),
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
      senderName: ownName,
      text: item.text,
      attachments: item.attachments,
      ts: item.ts,
      status: item.status,
      ...(item.progress !== undefined ? { progress: item.progress } : {}),
      onRetry: item.retry,
    })
  }

  const drop = useFileDrop(Boolean(connection), sendFiles)

  if (!open) return null

  return (
    <div
      data-local-input="true"
      className={
        fullPage
          ? 'relative flex h-full w-full min-h-0 flex-col bg-surface'
          : 'absolute inset-y-0 right-0 z-40 flex w-[min(92vw,340px)] flex-col border-l border-line bg-surface/95 shadow-2xl backdrop-blur-md'
      }
      onPointerDown={(event) => event.stopPropagation()}
      onPointerUp={(event) => event.stopPropagation()}
      {...drop.handlers}
    >
      <div className="flex h-11 shrink-0 items-center gap-2 border-b border-line-soft px-3">
        <MessageSquare size={15} className="text-accent" />
        <span className="text-[13px] font-medium">{t('chat.title')}</span>
        <span className="chip ml-1 !px-1.5 !py-0 text-[10px]">
          {remoteLabel}
        </span>
        {remoteId ? (
          <span className="hidden truncate font-mono text-[10px] text-muted sm:inline">
            {remoteId}
          </span>
        ) : null}
        <div className="ml-auto flex items-center gap-1">
          {callAvailable ? (
            <button
              type="button"
              onClick={onCall}
              disabled={callPhase !== 'idle'}
              className="flex items-center gap-1 rounded-md border border-line px-1.5 py-1 text-[10px] text-muted transition hover:border-accent/40 hover:text-ink disabled:opacity-50"
              title={callPhase === 'idle' ? t('call.start') : t('call.inCall')}
              aria-label={t('call.start')}
            >
              <Phone size={12} />
              {callPhase === 'idle' ? t('call.start') : t('call.inCall')}
            </button>
          ) : null}
          {onPopOut ? (
            <button
              type="button"
              onClick={onPopOut}
              className="grid h-7 w-7 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink"
              aria-label={t('session.openInTab')}
              title={t('session.openInTab')}
            >
              <ExternalLink size={14} />
            </button>
          ) : null}
          <button
            type="button"
            onClick={onClose}
            className="grid h-7 w-7 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink"
            aria-label={t('chat.close')}
            title={t('chat.close')}
          >
            <X size={15} />
          </button>
        </div>
      </div>

      <ChatFeed
        rows={rows}
        access={access}
        selfName={ownName}
        empty={t('chat.empty')}
        conversationKey={remoteId ?? remoteLabel}
      />

      <ChatComposer
        onSendText={sendText}
        onSendFiles={sendFiles}
        attachEnabled={Boolean(connection)}
        placeholder={t('chat.placeholder')}
        hint={hint}
      />

      {drop.dragging ? (
        <div className="pointer-events-none absolute inset-0 z-50 grid place-items-center border-2 border-dashed border-accent bg-surface/85 text-[12px] font-medium text-accent">
          {t('chat.dropHint')}
        </div>
      ) : null}
    </div>
  )
}
