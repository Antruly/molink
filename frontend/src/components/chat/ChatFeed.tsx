import { Download, Copy, Loader2 } from 'lucide-react'
import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from 'react'
import { createPortal } from 'react-dom'
import { useTranslation } from 'react-i18next'
import {
  CHAT_HISTORY_PAGE,
  formatTimeSeparator,
  isNearBottom,
  shouldInsertTimeSeparator,
  type ChatAttachment,
} from '../../lib/chat'
import type { ChatAttachmentAccess } from '../../lib/chat-access'
import { ChatBubble, type ChatFeedRow } from './ChatBubble'
import { copyText } from './chat-hooks'
import { ImageViewer, type ViewerImage } from './ImageViewer'

function TimeSeparator({ ts }: { ts: number }) {
  const label = formatTimeSeparator(ts)
  if (label === '') return null
  return (
    <div className="flex justify-center py-1">
      <span className="rounded-full bg-panel px-2 py-0.5 text-[9px] text-muted">
        {label}
      </span>
    </div>
  )
}

function ContextMenu({
  row,
  x,
  y,
  access,
  onClose,
  onNotice,
}: {
  row: ChatFeedRow
  x: number
  y: number
  access: ChatAttachmentAccess
  onClose: () => void
  onNotice: (message: string) => void
}) {
  const { t } = useTranslation()
  const menuRef = useRef<HTMLDivElement>(null)
  const previousFocus = useRef<HTMLElement | null>(null)

  useEffect(() => {
    previousFocus.current =
      document.activeElement instanceof HTMLElement
        ? document.activeElement
        : null
    menuRef.current?.focus()
    return () => previousFocus.current?.focus()
  }, [])

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape') onClose()
    }
    document.addEventListener('keydown', onKey)
    window.addEventListener('blur', onClose)
    return () => {
      document.removeEventListener('keydown', onKey)
      window.removeEventListener('blur', onClose)
    }
  }, [onClose])

  const copy = async () => {
    const text = row.text !== ''
      ? row.text
      : row.attachments.map((attachment) => attachment.name).join('\n')
    const ok = await copyText(text)
    onClose()
    if (!ok) onNotice(t('chat.copyFailed'))
  }

  const save = async (attachment: ChatAttachment) => {
    onClose()
    try {
      const ok = await access.save(attachment)
      if (!ok) onNotice(t('chat.saveCancelled'))
    } catch {
      onNotice(t('chat.saveFailed'))
    }
  }

  const width = 216
  const rows = 1 + Math.min(row.attachments.length, 6)
  const height = rows * 34 + 12
  const left = Math.max(8, Math.min(x, window.innerWidth - width - 8))
  const top = Math.max(8, Math.min(y, window.innerHeight - height - 8))

  return createPortal(
    <div
      data-local-input="true"
      className="fixed inset-0 z-[135]"
      onPointerDown={onClose}
    >
      <div
        ref={menuRef}
        tabIndex={-1}
        className="absolute overflow-hidden rounded-lg border border-line bg-surface py-1 text-[12px] shadow-2xl outline-none"
        style={{ left, top, width }}
        onPointerDown={(event) => event.stopPropagation()}
        onContextMenu={(event) => event.preventDefault()}
        role="menu"
      >
        <button
          type="button"
          onClick={() => void copy()}
          className="flex w-full items-center gap-2 px-3 py-2 text-left text-ink transition hover:bg-panel"
          role="menuitem"
        >
          <Copy size={13} className="text-muted" />
          {t('chat.copy')}
        </button>
        {row.attachments.slice(0, 6).map((attachment) => (
          <button
            key={attachment.id}
            type="button"
            onClick={() => void save(attachment)}
            className="flex w-full items-center gap-2 px-3 py-2 text-left text-ink transition hover:bg-panel"
            role="menuitem"
          >
            <Download size={13} className="shrink-0 text-muted" />
            <span className="min-w-0 truncate">
              {t('chat.saveAs')} · {attachment.name}
            </span>
          </button>
        ))}
      </div>
    </div>,
    document.body,
  )
}

/// The scrollable message list. It owns time separators, the unread pill,
/// history paging, the long-press/right-click menu and the image viewer so
/// both chat surfaces behave identically.
export function ChatFeed({
  rows,
  access,
  selfName,
  empty,
  conversationKey = '',
}: {
  rows: ChatFeedRow[]
  access: ChatAttachmentAccess
  selfName: string
  empty: string
  /// Reset scroll position and paging when the pane points at another peer.
  conversationKey?: string
}) {
  const { t } = useTranslation()
  const scrollRef = useRef<HTMLDivElement>(null)
  const rowsRef = useRef(rows)
  useEffect(() => {
    rowsRef.current = rows
  }, [rows])
  const prevCount = useRef(rows.length)
  const prevKey = useRef(conversationKey)
  const nearBottom = useRef(true)
  const prepend = useRef<{ height: number; top: number } | null>(null)

  const [older, setOlder] = useState(0)
  const [unseen, setUnseen] = useState(0)
  const [loadingMore, setLoadingMore] = useState(false)
  const loadingMoreRef = useRef(false)
  const [menu, setMenu] = useState<{ row: ChatFeedRow; x: number; y: number } | null>(null)
  const [viewer, setViewer] = useState<number | null>(null)
  const [notice, setNotice] = useState<string | null>(null)
  const noticeTimer = useRef(0)

  const notify = useCallback((message: string) => {
    setNotice(message)
    window.clearTimeout(noticeTimer.current)
    noticeTimer.current = window.setTimeout(() => setNotice(null), 3200)
  }, [])

  const scrollToEnd = useCallback((smooth = false) => {
    const element = scrollRef.current
    if (!element) return
    nearBottom.current = true
    setUnseen(0)
    if (smooth) {
      element.scrollTo({ top: element.scrollHeight, behavior: 'smooth' })
    } else {
      element.scrollTop = element.scrollHeight
    }
  }, [])

  const loadMore = useCallback(() => {
    const total = rowsRef.current.length
    if (loadingMoreRef.current || total - CHAT_HISTORY_PAGE - older <= 0) return
    loadingMoreRef.current = true
    setLoadingMore(true)
    window.setTimeout(() => {
      // Measure right before the page lands: a message that arrived while the
      // spinner was up may already have shifted the scroll height.
      const element = scrollRef.current
      if (element) {
        prepend.current = {
          height: element.scrollHeight,
          top: element.scrollTop,
        }
      }
      setOlder((current) => current + CHAT_HISTORY_PAGE)
      setLoadingMore(false)
      loadingMoreRef.current = false
    }, 120)
  }, [older])

  // A new peer: go back to the newest page and the bottom.
  useEffect(() => {
    if (prevKey.current === conversationKey) return
    prevKey.current = conversationKey
    setOlder(0)
    setUnseen(0)
    setMenu(null)
    setViewer(null)
    prevCount.current = rowsRef.current.length
    nearBottom.current = true
    window.requestAnimationFrame(() => scrollToEnd())
  }, [conversationKey, scrollToEnd])

  useLayoutEffect(() => {
    scrollToEnd()
    // The pane is mounted when it is opened; the first paint goes to newest.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  // Preserve the viewport when an older page is prepended.
  useLayoutEffect(() => {
    const element = scrollRef.current
    const saved = prepend.current
    if (!element || !saved) return
    element.scrollTop = element.scrollHeight - saved.height + saved.top
    prepend.current = null
  }, [older])

  useEffect(() => {
    const added = rows.length - prevCount.current
    prevCount.current = rows.length
    if (added <= 0) return
    if (nearBottom.current) {
      window.requestAnimationFrame(() => scrollToEnd())
      return
    }
    const appended = rows.slice(Math.max(0, rows.length - added))
    const incoming = appended.filter((row) => !row.mine).length
    if (incoming > 0) setUnseen((count) => count + incoming)
    // Keep the same top message on screen while new ones arrive below: adding
    // the same number of "older" rows pins the first visible index.
    setOlder((current) => Math.min(rows.length, current + added))
  }, [rows, rows.length, scrollToEnd])

  useEffect(() => {
    if (!notice) return
    return () => window.clearTimeout(noticeTimer.current)
  }, [notice])

  const images = useMemo<ViewerImage[]>(
    () =>
      rows.flatMap((row) =>
        row.attachments
          .filter((attachment) => attachment.kind === 'image')
          .map((attachment) => ({ key: row.key, attachment })),
      ),
    [rows],
  )

  const openImage = useCallback(
    (attachment: ChatAttachment) => {
      const index = images.findIndex(
        (item) => item.attachment.id === attachment.id,
      )
      if (index >= 0) setViewer(index)
    },
    [images],
  )

  const onScroll = () => {
    const element = scrollRef.current
    if (!element) return
    const near = isNearBottom(
      element.scrollTop,
      element.scrollHeight,
      element.clientHeight,
    )
    nearBottom.current = near
    if (near && unseen !== 0) setUnseen(0)
    if (element.scrollTop <= 24) loadMore()
  }

  const start = Math.max(0, rows.length - CHAT_HISTORY_PAGE - older)
  const visibleRows = rows.slice(start)
  const hasMore = start > 0

  const blocks: ReactNode[] = []
  let previous: ChatFeedRow | undefined
  let previousTs = 0
  for (const row of visibleRows) {
    const separator = shouldInsertTimeSeparator(previousTs, row.ts)
    if (separator) {
      blocks.push(<TimeSeparator key={`time-${row.key}`} ts={row.ts} />)
    }
    const showSender =
      !row.mine &&
      (previous === undefined ||
        previous.mine ||
        previous.senderName !== row.senderName ||
        separator)
    blocks.push(
      <ChatBubble
        key={row.key}
        row={row}
        access={access}
        selfName={selfName}
        showSender={showSender}
        onOpenImage={openImage}
        onMenu={(target, x, y) => setMenu({ row: target, x, y })}
        onNotice={notify}
      />,
    )
    previous = row
    previousTs = row.ts
  }

  return (
    <div className="relative min-h-0 flex-1">
      <div
        ref={scrollRef}
        onScroll={onScroll}
        className="h-full space-y-2 overflow-y-auto px-3 py-3"
      >
        {rows.length === 0 ? (
          <div className="rounded-xl border border-dashed border-line px-3 py-8 text-center text-[12px] leading-relaxed text-muted">
            {empty}
          </div>
        ) : (
          <>
            {hasMore ? (
              <div className="flex h-6 items-center justify-center">
                <button
                  type="button"
                  onClick={loadMore}
                  disabled={loadingMore}
                  className="inline-flex items-center rounded-full px-2 py-0.5 text-[10px] text-muted transition hover:text-ink disabled:opacity-70"
                >
                  {loadingMore ? (
                    <Loader2 size={11} className="mr-1 animate-spin" />
                  ) : null}
                  {loadingMore
                    ? t('chat.loading')
                    : t('chat.loadEarlier')}
                </button>
              </div>
            ) : null}
            {blocks}
          </>
        )}
      </div>

      {unseen > 0 ? (
        <button
          type="button"
          onClick={() => scrollToEnd(true)}
          className="absolute bottom-2 left-1/2 z-10 -translate-x-1/2 rounded-full bg-accent px-3 py-1 text-[10px] font-medium text-on-accent shadow-lg"
        >
          {t('chat.newMessages', { count: unseen })}
        </button>
      ) : null}

      {notice ? (
        <div className="pointer-events-none absolute bottom-10 left-1/2 z-10 -translate-x-1/2 whitespace-nowrap rounded-md bg-ink/85 px-2.5 py-1 text-[10px] text-surface shadow-lg">
          {notice}
        </div>
      ) : null}

      {menu ? (
        <ContextMenu
          row={menu.row}
          x={menu.x}
          y={menu.y}
          access={access}
          onClose={() => setMenu(null)}
          onNotice={notify}
        />
      ) : null}

      {viewer !== null && images.length > 0 ? (
        <ImageViewer
          images={images}
          startIndex={viewer}
          access={access}
          onClose={() => setViewer(null)}
          onNotice={notify}
        />
      ) : null}
    </div>
  )
}
