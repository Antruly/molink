import { FolderUp, MessageSquare, Monitor, Terminal } from 'lucide-react'
import type { SessionIntent } from '../../store/app'

/// One icon per `SESSION_MODES` entry. The list itself is React-free; this map
/// keeps the connect card and the recent-device shortcuts on the same icons.
export const MODE_ICONS: Record<SessionIntent, typeof Monitor> = {
  desktop: Monitor,
  files: FolderUp,
  chat: MessageSquare,
  terminal: Terminal,
}
