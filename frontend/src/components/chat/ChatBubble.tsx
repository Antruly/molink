import {
  Check,
  CircleAlert,
  Download,
  Loader2,
  Play,
  VideoOff,
} from 'lucide-react'
import { useEffect, useRef, useState, type PointerEvent as ReactPointerEvent } from 'react'
import { useTranslation } from 'react-i18next'
import {
  attachmentHasThumb,
  avatarColorFor,
  formatDuration,
  formatSize,
  formatTime,
  initialFor,
  pickThumbSource,
  type ChatAttachment,
} from '../../lib/chat'
import type { ChatAttachmentAccess } from '../../lib/chat-access'
import { AttachmentIcon } from './AttachmentIcon'

export type ChatDelivery = 'sending' | 'sent' | 'failed'

export interface ChatFeedRow {
  key: string
  mine: boolean
  senderName: string
  senderId?: string
  text: string
  attachments: ChatAttachment[]
  ts: number
  /// Optimistic state for a message this client just sent. Server messages
  /// have no state and render their delivery tick instead.
  status?: ChatDelivery
  /// 0..1 while an attachment uploads.
  progress?: number
  onRetry?: () => void
}

function useThumb(
  access: ChatAttachmentAccess,
  attachment: ChatAttachment,
): { url: string | null; loading: boolean; hasThumb: boolean } {
  const hasThumb = attachmentHasThumb(attachment)
  // A `data:` URL is already in the page; anything else names a host file the
  // access object has to pull before the bubble can paint it.
  const inline = pickThumbSource(attachment, null)
  const key = `${attachment.id}\u0000${attachment.thumb}\u0000${attachment.src}`
  // The descriptor is rebuilt by the parent on every render; keep the last
  // object for the async lookup but key all state by the primitive fields.
  const attachmentRef = useRef(attachment)
  const [state, setState] = useState<{ key: string; url: string | null }>({
    key,
    url: inline,
  })

  useEffect(() => {
    attachmentRef.current = attachment
  }, [attachment])

  const remoteOnly = hasThumb && inline === null
  useEffect(() => {
    if (!remoteOnly) return
    let alive = true
    access
      .thumbUrl(attachmentRef.current)
      .then((value) => {
        if (alive) {
          setState({
            key,
            url: pickThumbSource(attachmentRef.current, value),
          })
        }
      })
      .catch(() => {
        if (alive) setState({ key, url: null })
      })
    return () => {
      alive = false
    }
  }, [access, key, remoteOnly])

  // No reference at all: the file card, never a spinner.
  if (!hasThumb) return { url: null, loading: false, hasThumb: false }
  if (inline !== null) return { url: inline, loading: false, hasThumb: true }
  // A reference exists but the downloaded file has not resolved yet.
  if (state.key !== key) return { url: null, loading: true, hasThumb: true }
  return { url: state.url, loading: false, hasThumb: true }
}

function attachmentBox(
  width: number,
  height: number,
  maxWidth = 230,
  maxHeight = 230,
): { width: number; height: number } {
  if (width <= 0 || height <= 0) return { width: 180, height: 135 }
  const ratio = width / height
  let w = Math.min(maxWidth, width)
  let h = w / ratio
  if (h > maxHeight) {
    h = maxHeight
    w = h * ratio
  }
  return { width: Math.max(80, Math.round(w)), height: Math.max(60, Math.round(h)) }
}

function ImageAttachment({
  attachment,
  access,
  onOpen,
}: {
  attachment: ChatAttachment
  access: ChatAttachmentAccess
  onOpen: () => void
}) {
  const { t } = useTranslation()
  const { url, loading, hasThumb } = useThumb(access, attachment)
  const box = attachmentBox(attachment.w, attachment.h)

  if (url) {
    return (
      <button
        type="button"
        onClick={onOpen}
        className="relative block overflow-hidden rounded-xl bg-black/10"
        style={{ width: box.width, height: box.height }}
        title={attachment.name}
        aria-label={t('chat.openImage')}
      >
        <img
          src={url}
          alt={attachment.name}
          draggable={false}
          className="h-full w-full object-cover"
        />
      </button>
    )
  }
  if (hasThumb && loading) {
    return (
      <div
        className="grid place-items-center rounded-xl bg-black/10 text-white/70"
        style={{ width: box.width, height: box.height }}
      >
        <Loader2 size={18} className="animate-spin" />
      </div>
    )
  }
  // Nothing to paint: no local data URI and no thumb file. The click opens
  // the original in the full-screen viewer; it does not retry the preview.
  return (
    <button
      type="button"
      onClick={onOpen}
      className="flex max-w-[230px] items-center gap-2 rounded-xl bg-black/[0.06] px-2.5 py-2 text-left"
      title={attachment.name}
    >
      <AttachmentIcon name={attachment.name} size={34} />
      <span className="min-w-0">
        <span className="block truncate text-[12px] font-medium">
          {attachment.name}
        </span>
        <span className="block text-[10px] opacity-60">
          {t('chat.loadImage')}
        </span>
      </span>
    </button>
  )
}

function VideoAttachment({
  attachment,
  access,
}: {
  attachment: ChatAttachment
  access: ChatAttachmentAccess
}) {
  const { t } = useTranslation()
  const { url: thumb, loading } = useThumb(access, attachment)
  const [playing, setPlaying] = useState(false)
  const [videoUrl, setVideoUrl] = useState<string | null>(null)
  const [videoLoading, setVideoLoading] = useState(false)
  const [videoError, setVideoError] = useState(false)
  const [progress, setProgress] = useState(0)
  const box = attachmentBox(attachment.w, attachment.h, 250, 250)
  const duration = formatDuration(attachment.dur)

  const play = async () => {
    if (videoUrl || videoLoading) return
    setPlaying(true)
    setVideoLoading(true)
    setVideoError(false)
    try {
      const url = await access.originalUrl(attachment, (received, total) => {
        if (total > 0) setProgress(Math.min(1, received / total))
      })
      if (url) setVideoUrl(url)
      else setVideoError(true)
    } catch {
      setVideoError(true)
    } finally {
      setVideoLoading(false)
    }
  }

  if (playing && videoUrl) {
    return (
      <div className="overflow-hidden rounded-xl bg-black">
        <video
          src={videoUrl}
          controls
          autoPlay
          playsInline
          className="block max-h-[260px] max-w-full"
        />
      </div>
    )
  }
  return (
    <button
      type="button"
      onClick={() => void play()}
      className="relative block overflow-hidden rounded-xl bg-black/20 text-white"
      style={{ width: box.width, height: box.height }}
      title={attachment.name}
      aria-label={t('chat.playVideo')}
    >
      {thumb ? (
        <img
          src={thumb}
          alt=""
          draggable={false}
          className="h-full w-full object-cover opacity-90"
        />
      ) : (
        <span className="grid h-full w-full place-items-center bg-black/25">
          {loading ? (
            <Loader2 size={18} className="animate-spin" />
          ) : (
            <VideoOff size={22} className="opacity-70" />
          )}
        </span>
      )}
      <span className="absolute inset-0 grid place-items-center">
        {videoLoading ? (
          <span className="rounded-full bg-black/55 px-2 py-1 text-[10px]">
            {progress > 0
              ? t('chat.loadingPercent', { percent: Math.round(progress * 100) })
              : t('chat.loading')}
          </span>
        ) : (
          <span className="grid h-10 w-10 place-items-center rounded-full bg-black/50 backdrop-blur">
            <Play size={18} fill="currentColor" />
          </span>
        )}
      </span>
      {duration !== '' ? (
        <span className="absolute bottom-1.5 right-1.5 rounded bg-black/60 px-1.5 py-0.5 text-[10px] tabular-nums">
          {duration}
        </span>
      ) : null}
      {videoError ? (
        <span className="absolute inset-x-0 bottom-0 bg-black/65 px-1.5 py-1 text-[10px]">
          {t('chat.videoUnavailable')}
        </span>
      ) : null}
    </button>
  )
}

function FileAttachment({
  attachment,
  access,
  onNotice,
}: {
  attachment: ChatAttachment
  access: ChatAttachmentAccess
  onNotice?: (message: string) => void
}) {
  const { t } = useTranslation()
  const [saving, setSaving] = useState(false)
  const save = async () => {
    if (saving) return
    setSaving(true)
    try {
      const ok = await access.save(attachment)
      if (!ok) onNotice?.(t('chat.saveCancelled'))
    } catch {
      onNotice?.(t('chat.saveFailed'))
    } finally {
      setSaving(false)
    }
  }
  return (
    <div className="flex w-[230px] max-w-full items-center gap-2.5 rounded-xl bg-black/[0.05] p-2">
      <AttachmentIcon name={attachment.name} size={38} />
      <div className="min-w-0 flex-1">
        <div
          className="line-clamp-2 break-all text-[12px] font-medium leading-snug"
          title={attachment.name}
        >
          {attachment.name}
        </div>
        <div className="mt-0.5 text-[10px] opacity-60">
          {formatSize(attachment.size)}
        </div>
      </div>
      <button
        type="button"
        onClick={() => void save()}
        disabled={saving}
        className="grid h-7 w-7 shrink-0 place-items-center rounded-md text-current opacity-70 transition hover:bg-black/10 hover:opacity-100 disabled:opacity-40"
        title={t('chat.saveAs')}
        aria-label={t('chat.saveAs')}
      >
        {saving ? (
          <Loader2 size={14} className="animate-spin" />
        ) : (
          <Download size={14} />
        )}
      </button>
    </div>
  )
}

function StatusMark({ row }: { row: ChatFeedRow }) {
  const { t } = useTranslation()
  if (!row.mine) return null
  if (row.status === 'failed') {
    return (
      <button
        type="button"
        onClick={row.onRetry}
        className="ml-1 inline-flex items-center text-[#c0392b] transition hover:opacity-75"
        title={t('chat.retry')}
        aria-label={t('chat.retry')}
      >
        <CircleAlert size={12} />
      </button>
    )
  }
  if (row.status === 'sending') {
    return (
      <span className="ml-1 inline-flex items-center gap-0.5 opacity-60">
        <Loader2 size={10} className="animate-spin" />
        {row.progress !== undefined && row.progress > 0
          ? `${Math.round(row.progress * 100)}%`
          : null}
      </span>
    )
  }
  return (
    <span className="ml-1 inline-flex items-center opacity-60" title={t('chat.sent')}>
      <Check size={11} />
    </span>
  )
}

/// One WeChat-style bubble: avatar on the sender's side, a light-green bubble
/// for this client, white/grey for the peer, and the time underneath.
export function ChatBubble({
  row,
  access,
  selfName,
  showSender,
  onOpenImage,
  onMenu,
  onNotice,
}: {
  row: ChatFeedRow
  access: ChatAttachmentAccess
  selfName: string
  showSender: boolean
  onOpenImage: (attachment: ChatAttachment) => void
  onMenu: (row: ChatFeedRow, x: number, y: number) => void
  onNotice?: (message: string) => void
}) {
  const { t } = useTranslation()
  const who = row.mine ? selfName : row.senderName
  // Mine keep one colour across the optimistic row and its echo; the peer's
  // device id disambiguates two people with the same display name.
  const avatarColor = avatarColorFor(
    row.mine ? who : `${who}\u0000${row.senderId ?? ''}`,
  )
  const longPress = useRef(0)
  const longPressPoint = useRef<{ x: number; y: number } | null>(null)
  const suppressed = useRef(false)

  const clearPress = () => {
    window.clearTimeout(longPress.current)
    longPress.current = 0
  }
  const beginPress = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (event.pointerType === 'mouse') return
    longPressPoint.current = { x: event.clientX, y: event.clientY }
    longPress.current = window.setTimeout(() => {
      longPress.current = 0
      suppressed.current = true
      const point = longPressPoint.current
      if (point) onMenu(row, point.x, point.y)
    }, 520)
  }
  const movePress = (event: ReactPointerEvent<HTMLDivElement>) => {
    const point = longPressPoint.current
    if (!point || longPress.current === 0) return
    if (Math.hypot(event.clientX - point.x, event.clientY - point.y) > 8) {
      clearPress()
    }
  }

  const avatar = (
    <span
      className="grid h-7 w-7 shrink-0 place-items-center rounded-md text-[11px] font-semibold text-white"
      style={{ background: avatarColor }}
      aria-hidden="true"
    >
      {initialFor(who)}
    </span>
  )

  return (
    <div
      className={
        'flex items-start gap-2 ' + (row.mine ? 'justify-end' : 'justify-start')
      }
    >
      {row.mine ? null : avatar}
      <div
        className={
          'flex min-w-0 max-w-[78%] flex-col ' +
          (row.mine ? 'items-end' : 'items-start')
        }
      >
        {showSender && !row.mine ? (
          <span className="mb-0.5 max-w-full truncate px-1 text-[10px] text-muted">
            {row.senderName}
          </span>
        ) : null}
        <div
          onContextMenu={(event) => {
            event.preventDefault()
            clearPress()
            onMenu(row, event.clientX, event.clientY)
          }}
          onPointerDown={beginPress}
          onPointerMove={movePress}
          onPointerUp={clearPress}
          onPointerCancel={clearPress}
          onClickCapture={(event) => {
            if (!suppressed.current) return
            suppressed.current = false
            event.preventDefault()
            event.stopPropagation()
          }}
          className={
            'rounded-2xl px-3 py-2 text-[12px] leading-relaxed shadow-sm ' +
            (row.mine
              ? 'rounded-tr-[5px] bg-[#95EC69] text-[#14260f]'
              : 'rounded-tl-[5px] border border-black/5 bg-[#f2f4f6] text-[#15202b]')
          }
        >
          {row.attachments.length > 0 ? (
            <div className="flex flex-col gap-1.5">
              {row.attachments.map((attachment) => {
                if (attachment.kind === 'image') {
                  return (
                    <ImageAttachment
                      key={attachment.id}
                      attachment={attachment}
                      access={access}
                      onOpen={() => onOpenImage(attachment)}
                    />
                  )
                }
                if (attachment.kind === 'video') {
                  return (
                    <VideoAttachment
                      key={attachment.id}
                      attachment={attachment}
                      access={access}
                    />
                  )
                }
                return (
                  <FileAttachment
                    key={attachment.id}
                    attachment={attachment}
                    access={access}
                    onNotice={onNotice}
                  />
                )
              })}
            </div>
          ) : null}
          {row.text !== '' ? (
            <div
              className={
                'whitespace-pre-wrap break-words ' +
                (row.attachments.length > 0 ? 'mt-1.5' : '')
              }
            >
              {row.text}
            </div>
          ) : null}
          {row.attachments.length === 0 && row.text === '' ? (
            <span className="italic opacity-50">{t('chat.emptyMessage')}</span>
          ) : null}
        </div>
        <div
          className={
            'mt-0.5 flex items-center px-1 text-[9px] ' +
            (row.mine ? 'text-muted' : 'text-muted/80')
          }
        >
          <span>{formatTime(row.ts)}</span>
          <StatusMark row={row} />
        </div>
      </div>
      {row.mine ? avatar : null}
    </div>
  )
}
