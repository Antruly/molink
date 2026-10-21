import { shellInfo, windowAction, type WindowAction } from '../lib/shell'

interface Spot {
  className: string
  action: WindowAction
}

const EDGES: Spot[] = [
  { className: 'left-3 right-3 top-0 h-1.5 cursor-ns-resize', action: 'resize-top' },
  { className: 'left-3 right-3 bottom-0 h-1.5 cursor-ns-resize', action: 'resize-bottom' },
  { className: 'top-3 bottom-3 left-0 w-1.5 cursor-ew-resize', action: 'resize-left' },
  { className: 'top-3 bottom-3 right-0 w-1.5 cursor-ew-resize', action: 'resize-right' },
  { className: 'left-0 top-0 h-3 w-3 cursor-nwse-resize', action: 'resize-tl' },
  { className: 'right-0 top-0 h-3 w-3 cursor-nesw-resize', action: 'resize-tr' },
  { className: 'left-0 bottom-0 h-3 w-3 cursor-nesw-resize', action: 'resize-bl' },
  { className: 'right-0 bottom-0 h-3 w-3 cursor-nwse-resize', action: 'resize-br' },
]

/// Invisible resize handles for frameless shell windows. Native decorations are
/// gone, so the web layer provides the edges.
export function ResizeFrame() {
  if (!shellInfo.enabled || shellInfo.platform === 'macos') return null
  return (
    <div className="pointer-events-none fixed inset-0 z-40">
      {EDGES.map((spot) => (
        <div
          key={spot.action}
          className={'pointer-events-auto absolute ' + spot.className}
          onPointerDown={(event) => {
            event.preventDefault()
            windowAction(spot.action)
          }}
        />
      ))}
    </div>
  )
}
