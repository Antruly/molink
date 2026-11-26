import {
  ClipboardPaste,
  Copy,
  Download,
  FolderPlus,
  LogOut,
  Minus,
  Plus,
  Trash2,
  Upload,
} from 'lucide-react'
import { useEffect, useRef, useState, type ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import type { ChatMessage } from '../lib/client'
import { sessionConnection, useAppStore, type SessionTab } from '../store/app'
import { ChatPanel } from './ChatPanel'
import { FileTransferPanel, type FilePanelActions } from './FileTransferPanel'
import { FloatingToolbar, type ToolbarItem } from './FloatingToolbar'
import { TerminalPanel, type TerminalPanelActions } from './TerminalPanel'

/// One pane, mounted once by PaneHost. It reads the owner tab live from the
/// store, so a welcome arriving after the drawer opened still reaches the
/// panel without remounting it.
function usePaneOwner(paneId: string): {
  owner?: SessionTab
  standalone: boolean
  kind?: 'files' | 'chat' | 'terminal'
  ownerTabId: string
} {
  const pane = useAppStore((state) =>
    state.panes.find((item) => item.id === paneId),
  )
  const owner = useAppStore((state) => {
    const current = state.panes.find((item) => item.id === paneId)
    if (!current) return undefined
    return (
      state.tabs.find((tab) => tab.id === current.ownerTabId) ??
      state.tabs.find((tab) => tab.id === current.standaloneTabId)
    )
  })
  return {
    owner,
    standalone: pane?.standaloneTabId !== undefined,
    kind: pane?.kind,
    ownerTabId: pane?.ownerTabId ?? '',
  }
}

/// The panel plus, on its own tab, the floating toolbar that drives it.
function PaneFrame({
  standalone,
  title,
  items,
  children,
}: {
  standalone: boolean
  title: string
  items: ToolbarItem[]
  children: ReactNode
}) {
  // The wrappers stay at the same position in the React tree in both modes.
  // Promoting a drawer to its own tab only changes class names and adds the
  // toolbar sibling before the content; the panel element itself keeps its
  // place, so its component instance (and terminal socket / xterm buffer) is
  // never remounted.
  return (
    <div
      className={
        standalone
          ? 'relative flex h-full min-h-0 w-full flex-col'
          : 'contents'
      }
    >
      {standalone ? (
        <FloatingToolbar items={items} label={title} />
      ) : null}
      <div className={standalone ? 'min-h-0 flex-1' : 'contents'}>
        {children}
      </div>
    </div>
  )
}

function FilesPane({ paneId }: { paneId: string }) {
  const { t } = useTranslation()
  const { owner, standalone, ownerTabId } = usePaneOwner(paneId)
  const closePane = useAppStore((state) => state.closePane)
  const promotePane = useAppStore((state) => state.promotePane)
  const pushToast = useAppStore((state) => state.pushToast)
  const actions = useRef<FilePanelActions | null>(null)
  const connection = owner ? sessionConnection(ownerTabId) ?? null : null
  // The panel uploads through the same ids as the automatic clipboard sync;
  // pause that path while it is open, and restore it when the pane closes.
  useEffect(() => {
    const active = sessionConnection(ownerTabId)
    if (!active) return
    active.setFilePanelOpen(true)
    return () => active.setFilePanelOpen(false)
  }, [ownerTabId])
  const title = t('files.title')
  const disconnect = () => closePane(paneId)
  const items: ToolbarItem[] = [
    {
      key: 'upload',
      label: t('files.sendTo'),
      icon: <Upload size={12} />,
      onSelect: () => actions.current?.upload(),
    },
    {
      key: 'download',
      label: t('files.receiveFrom'),
      icon: <Download size={12} />,
      onSelect: () => actions.current?.download(),
    },
    {
      key: 'mkdir',
      label: t('files.newFolder'),
      icon: <FolderPlus size={12} />,
      onSelect: () => actions.current?.newFolder(),
    },
    {
      key: 'disconnect',
      label: t('session.disconnect'),
      icon: <LogOut size={12} />,
      onSelect: disconnect,
    },
  ]
  return (
    <PaneFrame standalone={standalone} title={title} items={items}>
      <FileTransferPanel
        open
        fullPage={standalone}
        connection={connection}
        onClose={disconnect}
        onError={(message) => {
          if (owner) pushToast(owner.id, message)
        }}
        {...(standalone ? {} : { onPopOut: () => promotePane(paneId) })}
        actionsRef={actions}
      />
    </PaneFrame>
  )
}

function TerminalPane({ paneId }: { paneId: string }) {
  const { t } = useTranslation()
  const { owner, standalone } = usePaneOwner(paneId)
  const closePane = useAppStore((state) => state.closePane)
  const promotePane = useAppStore((state) => state.promotePane)
  const actions = useRef<TerminalPanelActions | null>(null)
  const title = t('terminal.title')
  const disconnect = () => closePane(paneId)
  const items: ToolbarItem[] = [
    {
      key: 'copy',
      label: t('terminal.copy'),
      icon: <Copy size={12} />,
      onSelect: () => actions.current?.copy(),
    },
    {
      key: 'paste',
      label: t('terminal.paste'),
      icon: <ClipboardPaste size={12} />,
      onSelect: () => actions.current?.paste(),
    },
    {
      key: 'zoomOut',
      label: t('terminal.fontSmaller'),
      icon: <Minus size={12} />,
      onSelect: () => actions.current?.zoomOut(),
    },
    {
      key: 'zoomIn',
      label: t('terminal.fontLarger'),
      icon: <Plus size={12} />,
      onSelect: () => actions.current?.zoomIn(),
    },
    {
      key: 'disconnect',
      label: t('session.disconnect'),
      icon: <LogOut size={12} />,
      onSelect: disconnect,
    },
  ]
  return (
    <PaneFrame standalone={standalone} title={title} items={items}>
      <TerminalPanel
        host={owner?.host ?? ''}
        port={owner?.port ?? 0}
        token={owner?.token ?? ''}
        sessionToken={owner?.welcome?.session_token}
        fullPage={standalone}
        onClose={disconnect}
        {...(standalone ? {} : { onPopOut: () => promotePane(paneId) })}
        actionsRef={actions}
      />
    </PaneFrame>
  )
}

function ChatPane({ paneId }: { paneId: string }) {
  const { t } = useTranslation()
  const { owner, standalone, ownerTabId } = usePaneOwner(paneId)
  const closePane = useAppStore((state) => state.closePane)
  const promotePane = useAppStore((state) => state.promotePane)
  const [messages, setMessages] = useState<ChatMessage[]>([])
  const connection = sessionConnection(ownerTabId)

  // The panel is independent from the desktop view: it listens to the same
  // shared connection the owner tab uses, and keeps its own scroll position.
  useEffect(() => {
    if (!connection) return
    connection.requestChatHistory()
    return connection.addListener((event) => {
      if (event.kind === 'chat-history') {
        setMessages(event.messages)
      } else if (event.kind === 'chat') {
        setMessages((previous) => [...previous, event.message].slice(-200))
      }
    })
  }, [connection])

  const title = t('chat.title')
  const disconnect = () => closePane(paneId)
  const items: ToolbarItem[] = [
    {
      key: 'clear',
      label: t('chat.clear'),
      icon: <Trash2 size={12} />,
      onSelect: () => setMessages([]),
    },
    {
      key: 'disconnect',
      label: t('session.disconnect'),
      icon: <LogOut size={12} />,
      onSelect: disconnect,
    },
  ]
  return (
    <PaneFrame standalone={standalone} title={title} items={items}>
      <ChatPanel
        open
        fullPage={standalone}
        onClose={disconnect}
        messages={messages}
        onSend={(text) => connection?.sendChat(text)}
        remoteLabel={owner?.label ?? ''}
        remoteId={owner?.welcome?.device_id}
        selfSessionId={owner?.welcome?.session_id}
        {...(standalone ? {} : { onPopOut: () => promotePane(paneId) })}
      />
    </PaneFrame>
  )
}

export function PaneView({ paneId }: { paneId: string }) {
  const { kind } = usePaneOwner(paneId)
  if (kind === 'files') return <FilesPane paneId={paneId} />
  if (kind === 'terminal') return <TerminalPane paneId={paneId} />
  if (kind === 'chat') return <ChatPane paneId={paneId} />
  return null
}
