import { useEffect, useMemo, useRef, useState, type DragEvent } from 'react'

function hasFiles(event: DragEvent<HTMLElement>): boolean {
  const types = event.dataTransfer?.types
  if (!types) return false
  for (const type of Array.from(types)) {
    if (type === 'Files') return true
  }
  return false
}

/// Files from a clipboard event. A screenshot is exposed as a File with an
/// empty (or generic) name, which the chat sanitizer turns into `image.png`.
export function filesFromClipboard(data: DataTransfer | null): File[] {
  if (!data) return []
  const files: File[] = []
  for (const file of Array.from(data.files)) files.push(file)
  if (files.length === 0) {
    for (const item of Array.from(data.items)) {
      if (item.kind !== 'file') continue
      const file = item.getAsFile()
      if (file) files.push(file)
    }
  }
  return files
}

/// Drag-and-drop for the whole chat surface. The counter keeps a child's
/// dragleave from hiding the overlay while the pointer is still inside.
export function useFileDrop(
  enabled: boolean,
  onFiles: (files: File[]) => void,
): {
  dragging: boolean
  handlers: {
    onDragEnter: (event: DragEvent<HTMLElement>) => void
    onDragOver: (event: DragEvent<HTMLElement>) => void
    onDragLeave: (event: DragEvent<HTMLElement>) => void
    onDrop: (event: DragEvent<HTMLElement>) => void
  }
} {
  const [dragging, setDragging] = useState(false)
  const depth = useRef(0)
  // A drag that leaves the window can miss the last dragleave; reset on the
  // window-level end events so the overlay cannot stick.
  useEffect(() => {
    if (!dragging) return
    const reset = () => {
      depth.current = 0
      setDragging(false)
    }
    window.addEventListener('dragend', reset)
    window.addEventListener('drop', reset)
    return () => {
      window.removeEventListener('dragend', reset)
      window.removeEventListener('drop', reset)
    }
  }, [dragging])
  const handlers = useMemo(() => {
    const reset = () => {
      depth.current = 0
      setDragging(false)
    }
    return {
      onDragEnter: (event: DragEvent<HTMLElement>) => {
        if (!enabled || !hasFiles(event)) return
        event.preventDefault()
        event.stopPropagation()
        depth.current += 1
        setDragging(true)
      },
      onDragOver: (event: DragEvent<HTMLElement>) => {
        if (!enabled || !hasFiles(event)) return
        event.preventDefault()
        event.stopPropagation()
        event.dataTransfer.dropEffect = 'copy'
      },
      onDragLeave: (event: DragEvent<HTMLElement>) => {
        if (!enabled) return
        // `types` can already be empty by the time a leave arrives; while a
        // file drag is counted, always take the exit so the overlay closes.
        if (depth.current === 0 && !hasFiles(event)) return
        event.preventDefault()
        event.stopPropagation()
        depth.current = Math.max(0, depth.current - 1)
        if (depth.current === 0) setDragging(false)
      },
      onDrop: (event: DragEvent<HTMLElement>) => {
        if (!enabled || !hasFiles(event)) return
        event.preventDefault()
        event.stopPropagation()
        const files = Array.from(event.dataTransfer.files)
        reset()
        if (files.length > 0) onFiles(files)
      },
    }
  }, [enabled, onFiles])
  return { dragging, handlers }
}

/// Copy text from a secure context or a plain-http LAN page. The textarea
/// fallback needs to stay in the DOM only for the duration of the command.
export async function copyText(text: string): Promise<boolean> {
  try {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(text)
      return true
    }
  } catch {
    // Fall through to the selection-based copy.
  }
  try {
    const area = document.createElement('textarea')
    area.value = text
    area.setAttribute('readonly', '')
    area.style.position = 'fixed'
    area.style.left = '-9999px'
    document.body.appendChild(area)
    area.select()
    const ok = document.execCommand('copy')
    area.remove()
    return ok
  } catch {
    return false
  }
}
