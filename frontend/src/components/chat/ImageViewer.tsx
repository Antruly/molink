import {
  ChevronLeft,
  ChevronRight,
  Download,
  Loader2,
  RotateCcw,
  X,
  ZoomIn,
  ZoomOut,
} from 'lucide-react'
import { useCallback, useEffect, useRef, useState, type MouseEvent as ReactMouseEvent } from 'react'
import { createPortal } from 'react-dom'
import { useTranslation } from 'react-i18next'
import type { ChatAttachment } from '../../lib/chat'
import type { ChatAttachmentAccess } from '../../lib/chat-access'
import { AttachmentIcon } from './AttachmentIcon'

export interface ViewerImage {
  key: string
  attachment: ChatAttachment
}

/// Full-screen image viewer. It only receives the image list, so switching
/// left/right stays inside the conversation rather than a single message.
export function ImageViewer({
  images,
  startIndex,
  access,
  onClose,
  onNotice,
}: {
  images: ViewerImage[]
  startIndex: number
  access: ChatAttachmentAccess
  onClose: () => void
  onNotice?: (message: string) => void
}) {
  const { t } = useTranslation()
  const [index, setIndex] = useState(() =>
    Math.max(0, Math.min(startIndex, images.length - 1)),
  )
  const [scale, setScale] = useState(1)
  const [saving, setSaving] = useState(false)
  const rootRef = useRef<HTMLDivElement>(null)
  const previousFocus = useRef<HTMLElement | null>(null)

  // Keep the session's keyboard forwarder local while the viewer is open: the
  // overlay owns focus, so every key target is inside `data-local-input`.
  useEffect(() => {
    previousFocus.current =
      document.activeElement instanceof HTMLElement
        ? document.activeElement
        : null
    rootRef.current?.focus()
    return () => previousFocus.current?.focus()
  }, [])

  const image = images[index]
  // `images` is rebuilt from the message rows; key the load state by the
  // primitive fields so a parent re-render neither restarts the download nor
  // flashes the previous picture.
  const imageId = image?.attachment.id ?? ''
  const imageSrc = image?.attachment.src ?? ''
  const imageThumb = image?.attachment.thumb ?? ''
  const loadKey = `${index}\u0000${imageId}\u0000${imageSrc}\u0000${imageThumb}`
  const [loaded, setLoaded] = useState<{
    key: string
    url: string | null
    failed: boolean
    progress: number
  }>({ key: '', url: null, failed: false, progress: 0 })
  const current = loaded.key === loadKey
  const url = current ? loaded.url : null
  const failed = current ? loaded.failed : false
  const progress = current ? loaded.progress : 0

  const move = useCallback(
    (delta: number) => {
      if (images.length <= 1) return
      setIndex((currentIndex) => {
        const next = currentIndex + delta
        if (next < 0) return images.length - 1
        if (next >= images.length) return 0
        return next
      })
      setScale(1)
    },
    [images.length],
  )

  useEffect(() => {
    const target = images[index]
    if (!target) return
    let alive = true
    access
      .originalUrl(target.attachment, (received, total) => {
        if (!alive || total <= 0) return
        setLoaded((previous) =>
          previous.key === loadKey
            ? { ...previous, progress: Math.min(1, received / total) }
            : previous,
        )
      })
      .then(async (value) => {
        if (!alive) return
        if (value !== null) {
          setLoaded({
            key: loadKey,
            url: value,
            failed: false,
            progress: 1,
          })
          return
        }
        // An optimistic image may still be in the middle of its upload and
        // not registered as a local file yet; its thumbnail is still a valid
        // preview instead of an error panel.
        let thumb: string | null = null
        try {
          thumb = await access.thumbUrl(target.attachment)
        } catch {
          thumb = null
        }
        if (!alive) return
        setLoaded({
          key: loadKey,
          url: thumb,
          failed: thumb === null,
          progress: 1,
        })
      })
      .catch(() => {
        if (!alive) return
        setLoaded({ key: loadKey, url: null, failed: true, progress: 0 })
      })
    return () => {
      alive = false
    }
    // `loadKey` captures the index and the current attachment's fields; the
    // rebuilt `images` array itself is not a meaningful dependency.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [access, loadKey])

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        onClose()
      } else if (event.key === 'ArrowLeft') {
        move(-1)
      } else if (event.key === 'ArrowRight') {
        move(1)
      } else if (event.key === '+' || event.key === '=') {
        setScale((value) => Math.min(4, value + 0.5))
      } else if (event.key === '-') {
        setScale((value) => Math.max(1, value - 0.5))
      }
    }
    document.addEventListener('keydown', onKey)
    return () => document.removeEventListener('keydown', onKey)
  }, [move, onClose])

  if (!image) return null

  const save = async () => {
    if (saving) return
    setSaving(true)
    try {
      const ok = await access.save(image.attachment)
      if (!ok) onNotice?.(t('chat.saveCancelled'))
    } catch {
      onNotice?.(t('chat.saveFailed'))
    } finally {
      setSaving(false)
    }
  }

  const stop = (event: ReactMouseEvent) => event.stopPropagation()

  return createPortal(
    <div
      ref={rootRef}
      tabIndex={-1}
      data-local-input="true"
      className="fixed inset-0 z-[140] flex flex-col bg-black/85 text-white outline-none backdrop-blur-sm"
      onClick={onClose}
      role="dialog"
      aria-modal="true"
      aria-label={image.attachment.name}
    >
      <div
        className="flex h-12 shrink-0 items-center gap-2 border-b border-white/10 px-3"
        onClick={stop}
      >
        <AttachmentIcon name={image.attachment.name} size={28} />
        <span className="min-w-0 flex-1 truncate text-[12px]">
          {image.attachment.name}
        </span>
        <span className="text-[10px] text-white/60 tabular-nums">
          {index + 1} / {images.length}
        </span>
        <button
          type="button"
          onClick={() => void save()}
          disabled={saving}
          className="grid h-8 w-8 place-items-center rounded-md text-white/80 transition hover:bg-white/10 hover:text-white disabled:opacity-50"
          title={t('chat.saveAs')}
          aria-label={t('chat.saveAs')}
        >
          {saving ? (
            <Loader2 size={15} className="animate-spin" />
          ) : (
            <Download size={15} />
          )}
        </button>
        <button
          type="button"
          onClick={onClose}
          className="grid h-8 w-8 place-items-center rounded-md text-white/80 transition hover:bg-white/10 hover:text-white"
          title={t('chat.closeViewer')}
          aria-label={t('chat.closeViewer')}
        >
          <X size={16} />
        </button>
      </div>

      <div className="relative min-h-0 flex-1 overflow-auto" onClick={onClose}>
        <div
          className="flex min-h-full items-center justify-center p-6"
          onClick={stop}
        >
          {url ? (
            <img
              src={url}
              alt={image.attachment.name}
              draggable={false}
              onDoubleClick={() =>
                setScale((value) => (value >= 2 ? 1 : 2))
              }
              className="max-h-[78vh] max-w-[88vw] select-none object-contain transition-transform duration-150"
              style={{ transform: `scale(${scale})` }}
            />
          ) : failed ? (
            <div className="flex flex-col items-center gap-3 text-white/70">
              <AttachmentIcon name={image.attachment.name} size={56} />
              <span className="text-[12px]">{t('chat.imageUnavailable')}</span>
              <span className="max-w-[80vw] break-all text-[11px] text-white/50">
                {image.attachment.name}
              </span>
            </div>
          ) : (
            <div className="flex flex-col items-center gap-2 text-white/70">
              <Loader2 size={26} className="animate-spin" />
              <span className="text-[11px]">
                {progress > 0
                  ? t('chat.loadingPercent', {
                      percent: Math.round(progress * 100),
                    })
                  : t('chat.loading')}
              </span>
            </div>
          )}
        </div>
        {images.length > 1 ? (
          <>
            <button
              type="button"
              onClick={(event) => {
                event.stopPropagation()
                move(-1)
              }}
              className="absolute left-2 top-1/2 grid h-10 w-10 -translate-y-1/2 place-items-center rounded-full bg-black/45 text-white/85 transition hover:bg-black/70"
              aria-label={t('chat.previousImage')}
            >
              <ChevronLeft size={22} />
            </button>
            <button
              type="button"
              onClick={(event) => {
                event.stopPropagation()
                move(1)
              }}
              className="absolute right-2 top-1/2 grid h-10 w-10 -translate-y-1/2 place-items-center rounded-full bg-black/45 text-white/85 transition hover:bg-black/70"
              aria-label={t('chat.nextImage')}
            >
              <ChevronRight size={22} />
            </button>
          </>
        ) : null}
      </div>

      <div
        className="flex h-11 shrink-0 items-center justify-center gap-1 border-t border-white/10"
        onClick={stop}
      >
        <button
          type="button"
          onClick={() => setScale((value) => Math.max(1, value - 0.5))}
          className="grid h-8 w-8 place-items-center rounded-md text-white/80 transition hover:bg-white/10"
          aria-label={t('chat.zoomOut')}
          title={t('chat.zoomOut')}
        >
          <ZoomOut size={16} />
        </button>
        <span className="w-12 text-center text-[11px] text-white/70 tabular-nums">
          {Math.round(scale * 100)}%
        </span>
        <button
          type="button"
          onClick={() => setScale((value) => Math.min(4, value + 0.5))}
          className="grid h-8 w-8 place-items-center rounded-md text-white/80 transition hover:bg-white/10"
          aria-label={t('chat.zoomIn')}
          title={t('chat.zoomIn')}
        >
          <ZoomIn size={16} />
        </button>
        <button
          type="button"
          onClick={() => setScale(1)}
          className="grid h-8 w-8 place-items-center rounded-md text-white/80 transition hover:bg-white/10"
          aria-label={t('chat.zoomReset')}
          title={t('chat.zoomReset')}
        >
          <RotateCcw size={14} />
        </button>
      </div>
    </div>,
    document.body,
  )
}
