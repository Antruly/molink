import { useTranslation } from 'react-i18next'
import {
  intentSuffixKey,
  tabTitle,
  type SessionIntent,
} from '../lib/session-intent'
import { useAppStore, type SessionTab } from '../store/app'
import { PaneSlot } from './PaneSlot'
import { SessionChrome } from './SessionChrome'

/// The independent files/chat/terminal tab: window chrome around the pane
/// that PaneHost mounted. No canvas, no remote cursor and no picture controls
/// are rendered here at all.
export function PaneTabPage({ tab }: { tab: SessionTab }) {
  const { t } = useTranslation()
  const closeTab = useAppStore((state) => state.closeTab)
  const setTabMinimized = useAppStore((state) => state.setTabMinimized)
  const suffixKey = intentSuffixKey(tab.intent as SessionIntent)
  const title = tabTitle(
    tab.label,
    tab.intent as SessionIntent,
    suffixKey ? String(t(suffixKey)) : '',
  )

  if (!tab.paneId) {
    return (
      <div className="grid flex-1 place-items-center text-sm text-muted">
        {title}
      </div>
    )
  }

  return (
    <SessionChrome
      tab={tab}
      title={title}
      onMinimize={() => setTabMinimized(tab.id, true)}
      onClose={() => closeTab(tab.id)}
    >
      <PaneSlot
        paneId={tab.paneId}
        className="relative h-full min-h-0 w-full"
      />
    </SessionChrome>
  )
}
