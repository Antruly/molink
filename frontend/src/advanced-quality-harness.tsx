/**
 * Test-only page for tools/verify_advanced_quality.mjs.
 *
 * It mounts the real QualityPanel over a "remote surface" that records every
 * pointer/wheel event reaching it, and wires the real 300 ms pump. The page is
 * generated/removed by the verification script; it is not part of the app.
 */
import { useEffect, useMemo, useRef, useState } from 'react'
import { createRoot } from 'react-dom/client'
import { QualityPanel } from './components/QualityPanel'
import './i18n'
import { shouldForwardKeys } from './lib/key-combos'
import {
  advancedPresetMessage,
  createAdvancedPresetPump,
  defaultAdvancedParams,
  type AdvancedQualityParams,
} from './lib/session-presets'
import './index.css'

declare global {
  interface Window {
    __sentPresets: Array<Record<string, unknown>>
    __remoteInput: Array<Record<string, unknown>>
    __remoteKeys: string[]
    __panelOpen: boolean
  }
}

window.__sentPresets = []
window.__remoteInput = []
window.__remoteKeys = []
window.__panelOpen = false

function Harness() {
  const [params, setParams] = useState<AdvancedQualityParams>(() =>
    defaultAdvancedParams('balanced'),
  )
  const [panelOpen, setPanelOpen] = useState(false)
  const [remote, setRemote] = useState(0)
  const [lastKind, setLastKind] = useState('-')
  const [lastSent, setLastSent] = useState<Record<string, unknown> | null>(null)
  const first = useRef(true)
  // Built once per mount; reading a ref during render is not allowed, so the
  // pump is memoised instead of being parked in a ref.
  const pump = useMemo(
    () =>
      createAdvancedPresetPump((message) => {
        window.__sentPresets.push(message as unknown as Record<string, unknown>)
        setLastSent(message as unknown as Record<string, unknown>)
      }),
    [],
  )

  // Mirrors SessionView: a change is debounced, not sent per pixel.
  useEffect(() => {
    if (first.current) {
      first.current = false
      return
    }
    pump.update(advancedPresetMessage(params))
  }, [params, pump])

  // Mirrors the keyboard gate: an open toolbar panel keeps keys local.
  useEffect(() => {
    window.__panelOpen = panelOpen
    const onKey = (event: KeyboardEvent) => {
      const target = event.target as HTMLElement | null
      const localTarget =
        target?.closest?.('[data-local-input]') !== null &&
        target?.closest?.('[data-local-input]') !== undefined
      if (
        shouldForwardKeys({
          hasFocus: true,
          visible: true,
          controlEnabled: true,
          localTarget,
          settingsOpen: false,
          localPanelOpen: window.__panelOpen,
        })
      ) {
        window.__remoteKeys.push(event.key)
      }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [panelOpen])

  return (
    <div
      id="remote-surface"
      style={{ position: 'fixed', inset: 0, background: '#1b2433' }}
      onPointerDown={(event) => {
        window.__remoteInput.push({ k: 'down', x: event.clientX, y: event.clientY })
        setRemote((value) => value + 1)
        setLastKind('down')
      }}
      onPointerMove={(event) => {
        window.__remoteInput.push({ k: 'move', x: event.clientX, y: event.clientY })
        setRemote((value) => value + 1)
        setLastKind('move')
      }}
      onWheel={() => {
        window.__remoteInput.push({ k: 'wheel' })
        setRemote((value) => value + 1)
        setLastKind('wheel')
      }}
    >
      <div
        id="harness-status"
        style={{
          position: 'fixed',
          right: 12,
          top: 12,
          zIndex: 60,
          padding: '6px 10px',
          borderRadius: 8,
          background: '#050b12',
          color: '#9af2df',
          font: '14px/1.5 monospace',
        }}
      >
        fps={params.fps} sent={String(lastSent?.fps ?? '-')} remote={remote}{' '}
        last={lastKind} panel={panelOpen ? 'open' : 'closed'}{' '}
        <button
          type="button"
          onPointerDown={(event) => event.stopPropagation()}
          onClick={() => {
            window.__remoteInput = []
            setRemote(0)
            setLastKind('-')
          }}
          style={{ marginLeft: 8 }}
        >
          reset
        </button>
      </div>
      <div
        style={{ position: 'fixed', top: 12, left: 12, zIndex: 40 }}
        onPointerDown={(event) => event.stopPropagation()}
        onPointerUp={(event) => event.stopPropagation()}
        onPointerMove={(event) => event.stopPropagation()}
        onWheel={(event) => event.stopPropagation()}
        onContextMenu={(event) => event.stopPropagation()}
      >
        <QualityPanel
          advanced
          preset="balanced"
          advancedParams={params}
          codecOptions={['av1', 'vp9', 'vp8', 'h264']}
          onAdvancedChange={(patch) =>
            setParams((current) => ({ ...current, ...patch }))
          }
          onBackToPreset={() => undefined}
          onPreset={() => undefined}
          onOpenChange={setPanelOpen}
        />
      </div>
    </div>
  )
}

createRoot(document.getElementById('root')!).render(<Harness />)
