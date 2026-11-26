// A pane is a React root that survives tab switches and minimise. This pins
// the DOM half of that promise: minimising only moves the pane's container to
// the off-screen holder, then back into a slot. The same container object is
// reused, so the terminal buffer / scroll position behind it is never rebuilt.
import assert from 'node:assert/strict'
import test from 'node:test'

class FakeElement {
  constructor(tag = 'div') {
    this.tagName = tag
    this.parentElement = null
    this.children = []
    this.style = {}
    this.attributes = {}
    this.isConnected = false
    this.className = ''
  }

  appendChild(child) {
    if (child.parentElement !== null) child.parentElement.removeChild(child)
    this.children.push(child)
    child.parentElement = this
    child.isConnected = this.isConnected
    return child
  }

  removeChild(child) {
    const index = this.children.indexOf(child)
    if (index >= 0) this.children.splice(index, 1)
    child.parentElement = null
    child.isConnected = false
    return child
  }

  setAttribute(name, value) {
    this.attributes[name] = String(value)
  }
}

const body = new FakeElement('body')
body.isConnected = true
globalThis.document = {
  body,
  createElement: (tag) => new FakeElement(tag),
}

const {
  registerPaneContainer,
  registerPaneSlot,
} = await import('../src/lib/pane-slots.ts')

function connectedSlot() {
  const slot = new FakeElement('div')
  slot.isConnected = true
  return slot
}

test('minimising moves a pane off-screen instead of unmounting it', () => {
  const id = 'pane-minimise-test'
  const container = new FakeElement('div')
  registerPaneContainer(id, container)

  // No tab shows this pane right now: it lives in the off-screen holder.
  const holder = container.parentElement
  assert.ok(holder)
  assert.equal(holder.isConnected, true)
  assert.equal(holder.style.visibility, 'hidden')
  // `display: none` would leave xterm with no measurable cell.
  assert.notEqual(holder.style.display, 'none')

  // The tab opens: the pane container moves into the tab's slot.
  const slot = connectedSlot()
  registerPaneSlot(id, slot)
  assert.equal(container.parentElement, slot)

  // Minimise: the tab page unmounts and unregisters its slot. The container
  // must return to the holder, not be destroyed or replaced.
  registerPaneSlot(id, null)
  assert.equal(container.parentElement, holder)
  assert.equal(container.isConnected, true)

  // Restore: the same object is appended to a fresh slot, so the component
  // instance behind it (xterm scrollback, file list scroll) is untouched.
  const restored = connectedSlot()
  registerPaneSlot(id, restored)
  assert.equal(container.parentElement, restored)
  assert.equal(container.children.length, 0)
})

test('two panes keep separate containers while both are hidden', () => {
  const first = new FakeElement('div')
  const second = new FakeElement('div')
  registerPaneContainer('pane-a', first)
  registerPaneContainer('pane-b', second)
  assert.notEqual(first.parentElement, null)
  assert.equal(first.parentElement, second.parentElement)

  const slot = connectedSlot()
  registerPaneSlot('pane-a', slot)
  assert.equal(first.parentElement, slot)
  // b stays hidden while a is shown.
  assert.notEqual(second.parentElement, slot)
  assert.equal(second.parentElement.style.visibility, 'hidden')
})
