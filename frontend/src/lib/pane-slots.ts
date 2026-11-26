/// Panes (chat, files, terminal) are mounted once at the app root, not inside
/// the tab that happens to show them. Moving one from a desktop drawer to its
/// own tab must not rebuild it: a terminal would lose its process, the file
/// panel would reconnect and chat would lose its scroll position.
///
/// The React tree for a pane lives in its own root (see PaneHost). The root's
/// container element is a plain DOM node, and these helpers only move that
/// node between slot elements. Because React never owns the container's
/// parent, moving it cannot confuse the reconciler.
const containers = new Map<string, HTMLElement>()
const slots = new Map<string, HTMLElement>()
let holder: HTMLElement | null = null

/// Hidden panes stay in the document, off-screen and invisible, rather than
/// `display: none`: an xterm instance needs a measurable element to refit, and
/// scroll positions survive visibility changes.
function hiddenHolder(): HTMLElement {
  if (holder?.isConnected) return holder
  holder = document.createElement('div')
  holder.setAttribute('data-pane-holder', 'true')
  holder.style.position = 'fixed'
  holder.style.left = '-20000px'
  holder.style.top = '0'
  holder.style.width = '1200px'
  holder.style.height = '800px'
  holder.style.visibility = 'hidden'
  holder.style.pointerEvents = 'none'
  holder.style.overflow = 'hidden'
  document.body.appendChild(holder)
  return holder
}

function place(id: string): void {
  const container = containers.get(id)
  if (!container) return
  const slot = slots.get(id)
  if (slot?.isConnected) {
    if (container.parentElement !== slot) slot.appendChild(container)
    return
  }
  const home = hiddenHolder()
  if (container.parentElement !== home) home.appendChild(container)
}

export function registerPaneContainer(
  id: string,
  element: HTMLElement | null,
): void {
  if (element) containers.set(id, element)
  else containers.delete(id)
  place(id)
}

export function registerPaneSlot(id: string, element: HTMLElement | null): void {
  if (element) slots.set(id, element)
  else slots.delete(id)
  place(id)
}
