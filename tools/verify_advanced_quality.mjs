#!/usr/bin/env node
/**
 * Real-browser acceptance for the advanced quality panel.
 *
 * Starts Vite with a throw-away harness page (the real QualityPanel + the
 * real 300 ms pump), drives Chrome over CDP with actual mouse coordinates,
 * and checks:
 *   a) dragging the fps slider changes its value;
 *   b) ~300 ms later the harness sent session-preset{preset:"custom",fps:...};
 *   c) the pointer drag produced no events on the remote surface;
 *   d) keys stay local while the panel is open;
 *   e) the slider hit-test at its own coordinates is the slider itself.
 *
 * Usage: node tools/verify_advanced_quality.mjs
 */
import { spawn } from 'node:child_process'
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const frontend = path.join(repo, 'frontend')
const html = path.join(frontend, 'advanced-quality-harness.html')
const tsx = path.join(frontend, 'src', 'advanced-quality-harness.tsx')
const vitePort = 1427
const profile = mkdtempSync(path.join(tmpdir(), 'molink-adv-chrome-'))
const children = []

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

function start(cmd, args, options) {
  const child = spawn(cmd, args, {
    stdio: ['ignore', 'pipe', 'pipe'],
    ...options,
  })
  children.push(child)
  return child
}

async function waitFor(check, what, timeoutMs = 20000) {
  const deadline = Date.now() + timeoutMs
  let last
  while (Date.now() < deadline) {
    try {
      last = await check()
      if (last) return last
    } catch (error) {
      last = error
    }
    await sleep(150)
  }
  throw new Error(`timed out waiting for ${what}: ${last}`)
}

function cleanup() {
  for (const child of children.reverse()) {
    try {
      child.kill('SIGTERM')
    } catch {
      // already gone
    }
  }
  try {
    rmSync(profile, { recursive: true, force: true })
  } catch {
    // best effort
  }
}
process.on('exit', cleanup)
process.on('SIGINT', () => process.exit(130))

class Cdp {
  constructor(ws) {
    this.ws = ws
    this.next = 1
    this.pending = new Map()
    ws.addEventListener('message', (event) => {
      const message = JSON.parse(String(event.data))
      if (!message.id) return
      const entry = this.pending.get(message.id)
      if (!entry) return
      this.pending.delete(message.id)
      if (message.error) entry.reject(new Error(JSON.stringify(message.error)))
      else entry.resolve(message.result)
    })
  }

  send(method, params = {}) {
    const id = this.next++
    this.ws.send(JSON.stringify({ id, method, params }))
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id)
        reject(new Error(`CDP ${method} timed out`))
      }, 10000)
      this.pending.set(id, {
        resolve: (value) => {
          clearTimeout(timer)
          resolve(value)
        },
        reject: (error) => {
          clearTimeout(timer)
          reject(error)
        },
      })
    })
  }

  async eval(expression) {
    const result = await this.send('Runtime.evaluate', {
      expression,
      returnByValue: true,
      awaitPromise: true,
    })
    if (result.exceptionDetails) {
      throw new Error(result.exceptionDetails.text)
    }
    return result.result.value
  }

  async mouse(type, x, y, extra = {}) {
    await this.send('Input.dispatchMouseEvent', {
      type,
      x,
      y,
      button: type === 'mouseMoved' && extra.buttons ? 'left' : 'left',
      buttons: extra.buttons ?? (type === 'mouseReleased' ? 0 : 1),
      clickCount: type === 'mouseMoved' ? 0 : 1,
    })
  }

  async click(x, y) {
    await this.mouse('mousePressed', x, y)
    await this.mouse('mouseReleased', x, y, { buttons: 0 })
  }

  async rect(selector) {
    return this.eval(`(() => {
      const el = document.querySelector(${JSON.stringify(selector)})
      if (!el) return null
      const r = el.getBoundingClientRect()
      return { x: r.x, y: r.y, w: r.width, h: r.height }
    })()`)
  }

  async key(key, code) {
    await this.send('Input.dispatchKeyEvent', {
      type: 'keyDown',
      key,
      code,
      windowsVirtualKeyCode: key.toUpperCase().charCodeAt(0),
    })
    await this.send('Input.dispatchKeyEvent', { type: 'keyUp', key, code })
  }
}

const log = (message) => console.error(`[verify] ${message}`)

async function main() {
  if (!existsSync(html) || !existsSync(tsx)) {
    throw new Error('harness files are missing (they are generated alongside this script)')
  }
  log('starting vite')
  const vite = start(
    path.join(frontend, 'node_modules', '.bin', 'vite'),
    ['--host', '127.0.0.1', '--port', String(vitePort), '--strictPort'],
    { cwd: frontend },
  )
  let viteLog = ''
  vite.stderr.on('data', (chunk) => {
    viteLog += String(chunk)
  })
  await waitFor(
    () =>
      fetch(`http://127.0.0.1:${vitePort}/advanced-quality-harness.html`)
        .then((response) => response.ok)
        .catch(() => false),
    `vite: ${viteLog.slice(-400)}`,
  )

  log('vite ready; starting chrome')
  const chrome = start(
    'google-chrome',
    [
      '--headless=new',
      '--disable-gpu',
      '--no-sandbox',
      '--disable-dev-shm-usage',
      '--window-size=1000,760',
      '--remote-debugging-port=0',
      `--user-data-dir=${profile}`,
      `http://127.0.0.1:${vitePort}/advanced-quality-harness.html`,
    ],
    {},
  )
  chrome.stderr.on('data', () => undefined)

  const portFile = path.join(profile, 'DevToolsActivePort')
  const port = await waitFor(
    () => {
      if (!existsSync(portFile)) return false
      const text = readFileSync(portFile, 'utf8').trim().split('\n')
      return Number(text[0]) || false
    },
    'chrome DevTools port',
  )
  const target = await waitFor(async () => {
    const list = await fetch(`http://127.0.0.1:${port}/json/list`).then((r) =>
      r.json(),
    )
    return list.find((item) => item.type === 'page' && item.webSocketDebuggerUrl)
  }, 'harness page target')

  log(`chrome target ${target.url}`)
  const ws = new WebSocket(target.webSocketDebuggerUrl)
  await new Promise((resolve, reject) => {
    ws.addEventListener('open', resolve, { once: true })
    ws.addEventListener('error', reject, { once: true })
  })
  const cdp = new Cdp(ws)
  await cdp.send('Runtime.enable')
  await cdp.send('Page.enable')
  log('waiting for the toolbar button')
  await waitFor(
    () => cdp.eval(`document.querySelector('button[aria-haspopup="dialog"]') !== null`),
    'quality toolbar button',
  )
  log('toolbar button rendered')
  // Give React/CSS one paint.
  await sleep(250)

  const toggle = await cdp.rect('button[aria-haspopup="dialog"]')
  await cdp.click(toggle.x + toggle.w / 2, toggle.y + toggle.h / 2)
  log('clicked the toolbar button; waiting for the panel')
  await waitFor(
    () => cdp.eval(`document.querySelector('[data-advanced-panel="true"]') !== null`),
    'advanced panel',
  )
  log('advanced panel open')

  const before = Number(
    await cdp.eval(`document.querySelector('[data-control="fps"]').value`),
  )
  const slider = await cdp.rect('[data-control="fps"]')
  const centerY = slider.y + slider.h / 2
  // A native range maps the thumb centre from 9px to width-9px. Start on the
  // thumb so this is a real drag, not a track click.
  const startX = slider.x + 9 + ((before - 5) / (60 - 5)) * (slider.w - 18)
  const endX = slider.x + slider.w * 0.86

  // Hit-test exactly where the drag starts: the pointer must reach the slider
  // itself, not an invisible overlay.
  const hit = await cdp.eval(`(() => {
    const el = document.elementFromPoint(${startX}, ${centerY})
    return el ? (el.closest('[data-control="fps"]') ? 'slider' : el.tagName) : 'none'
  })()`)
  if (hit !== 'slider') throw new Error(`slider hit-test intercepted by ${hit}`)

  await cdp.eval(`window.__remoteInput = []`)
  await cdp.mouse('mousePressed', startX, centerY)
  for (let step = 1; step <= 8; step += 1) {
    const x = startX + ((endX - startX) * step) / 8
    await cdp.mouse('mouseMoved', x, centerY, { buttons: 1 })
  }
  await cdp.mouse('mouseReleased', endX, centerY, { buttons: 0 })
  await sleep(50)

  const after = Number(
    await cdp.eval(`document.querySelector('[data-control="fps"]').value`),
  )
  if (!Number.isFinite(after) || after === before) {
    throw new Error(`dragging did not change fps (${before} -> ${after})`)
  }

  // The debounce is 300 ms; allow one event-loop tick either side.
  await sleep(450)
  const sent = await cdp.eval('window.__sentPresets')
  const last = sent[sent.length - 1]
  if (!last) throw new Error('no session-preset was sent')
  if (last.preset !== 'custom') {
    throw new Error(`preset was ${last.preset}, expected custom`)
  }
  if (last.fps !== after) {
    throw new Error(`sent fps ${last.fps} != slider value ${after}`)
  }

  const remoteInput = await cdp.eval('window.__remoteInput')
  if (remoteInput.length !== 0) {
    throw new Error(
      `the slider drag leaked ${remoteInput.length} input event(s): ` +
        JSON.stringify(remoteInput.slice(0, 5)),
    )
  }

  // Keys stay local while the panel is open.
  await cdp.eval(`document.querySelector('[data-control="fps"]').blur();
                  window.__remoteKeys = []`)
  await cdp.key('z', 'KeyZ')
  await sleep(80)
  let keys = await cdp.eval('window.__remoteKeys')
  if (keys.length !== 0) {
    throw new Error(`panel-open key reached the remote: ${JSON.stringify(keys)}`)
  }

  // Closing the panel restores forwarding.
  await cdp.click(toggle.x + toggle.w / 2, toggle.y + toggle.h / 2)
  await waitFor(
    () => cdp.eval(`document.querySelector('[data-advanced-panel="true"]') === null`),
    'panel close',
  )
  await cdp.eval(`document.body.focus(); window.__remoteKeys = []`)
  await cdp.key('z', 'KeyZ')
  await sleep(80)
  keys = await cdp.eval('window.__remoteKeys')
  if (!keys.includes('z')) {
    throw new Error(`keys did not resume after closing the panel: ${JSON.stringify(keys)}`)
  }

  const report = {
    fpsBefore: before,
    fpsAfter: after,
    sent: last,
    remoteInputEvents: remoteInput.length,
    panelOpenKeysForwarded: 0,
    panelClosedKeysForwarded: keys.length,
    hitTest: hit,
  }
  console.log(JSON.stringify(report, null, 2))
  ws.close()
  // The spawned vite/chrome keep the event loop alive; exiting also runs the
  // cleanup handler that stops them.
  process.exit(0)
}

main().catch((error) => {
  console.error(`FAIL: ${error.message}`)
  process.exit(1)
})
