import { createElement, useEffect } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { registerPaneContainer } from '../lib/pane-slots'
import { useAppStore, type SessionPane } from '../store/app'
import { PaneView } from './PaneView'

interface MountedPane {
  container: HTMLDivElement
  root: Root
}

const mounted = new Map<string, MountedPane>()

function syncPanes(panes: SessionPane[]): void {
  const live = new Set(panes.map((pane) => pane.id))
  for (const [id, entry] of mounted) {
    if (live.has(id)) continue
    // A close runs inside the pane's own event handler. Unmounting that root
    // synchronously would be a render-phase update; defer it one microtask.
    mounted.delete(id)
    registerPaneContainer(id, null)
    queueMicrotask(() => {
      if (useAppStore.getState().panes.some((pane) => pane.id === id)) return
      entry.root.unmount()
      entry.container.remove()
    })
  }
  for (const pane of panes) {
    let entry = mounted.get(pane.id)
    if (!entry) {
      const container = document.createElement('div')
      container.setAttribute('data-pane', pane.kind)
      const root = createRoot(container)
      root.render(createElement(PaneView, { paneId: pane.id }))
      entry = { container, root }
      mounted.set(pane.id, entry)
      registerPaneContainer(pane.id, container)
    }
    // A standalone page fills its tab. An embedded drawer's panel positions
    // itself against the session surface, so its container is layout-neutral.
    const className =
      pane.standaloneTabId !== undefined
        ? 'relative flex h-full min-h-0 w-full flex-col'
        : 'contents'
    if (entry.container.className !== className) {
      entry.container.className = className
    }
    registerPaneContainer(pane.id, entry.container)
  }
}

/// Mounts every open pane once. Panes are React roots of their own, so moving
/// one between a drawer slot and its tab slot (or hiding it while another tab
/// is active) never remounts the terminal/file/chat component.
export function PaneHost() {
  useEffect(() => {
    const apply = () => syncPanes(useAppStore.getState().panes)
    apply()
    const unsubscribe = useAppStore.subscribe((state, previous) => {
      if (state.panes !== previous.panes) apply()
    })
    return () => {
      unsubscribe()
      for (const [id, entry] of mounted) {
        entry.root.unmount()
        entry.container.remove()
        registerPaneContainer(id, null)
      }
      mounted.clear()
    }
  }, [])
  return null
}
