import { useEffect, useRef } from 'react'
import { registerPaneSlot } from '../lib/pane-slots'

/// The DOM hole a pane is moved into. It renders no React children: the pane
/// itself lives in PaneHost's own root and is appended here.
export function PaneSlot({
  paneId,
  className = 'contents',
}: {
  paneId: string
  className?: string
}) {
  const ref = useRef<HTMLDivElement>(null)
  useEffect(() => {
    const element = ref.current
    if (!element) return
    registerPaneSlot(paneId, element)
    return () => registerPaneSlot(paneId, null)
  }, [paneId])
  return <div ref={ref} data-pane-slot={paneId} className={className} />
}
