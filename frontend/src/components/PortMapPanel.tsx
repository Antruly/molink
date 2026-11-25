import { ArrowLeftRight, Plus, Trash2 } from 'lucide-react'
import { useCallback, useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import {
  ApiError,
  createPortMapping,
  deletePortMapping,
  fetchPortMappings,
  patchPortMapping,
} from '../lib/api'
import {
  EMPTY_PORTMAP_FORM,
  PORTMAP_PRESETS,
  applyPortmapPreset,
  buildPortmapInput,
  formatPortmapBytes,
  mappingStatus,
  portForwardEnabled,
  portForwardSaveValues,
  portmapErrorKey,
  type PortMapping,
  type PortmapForm,
} from '../lib/portmap'
import { useAppStore } from '../store/app'

function errorCode(caught: unknown): string {
  return caught instanceof ApiError ? caught.code : 'unknown'
}

function StatusDot({ status }: { status: ReturnType<typeof mappingStatus> }) {
  const colour =
    status === 'listening'
      ? 'bg-emerald-500'
      : status === 'error'
        ? 'bg-red-500'
        : 'bg-muted/60'
  return (
    <span
      aria-hidden="true"
      className={`inline-block h-2 w-2 shrink-0 rounded-full ${colour}`}
    />
  )
}

function MappingSwitch({
  enabled,
  disabled,
  label,
  onChange,
}: {
  enabled: boolean
  disabled?: boolean
  label: string
  onChange: (next: boolean) => void
}) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={enabled}
      aria-label={label}
      disabled={disabled}
      onClick={() => onChange(!enabled)}
      className={
        'relative h-5 w-9 shrink-0 rounded-full border transition ' +
        (enabled
          ? 'border-accent/50 bg-accent/80'
          : 'border-line bg-panel') +
        (disabled ? ' opacity-40' : '')
      }
    >
      <span
        className={
          'absolute top-0.5 rounded-full bg-surface shadow transition-all ' +
          (enabled ? 'left-[18px]' : 'left-0.5')
        }
        style={{ width: '0.875rem', height: '0.875rem' }}
      />
    </button>
  )
}

/// Controller-side port mappings. The core owns the local listener, so this
/// panel talks only to loopback (`/api/portmap`); the host still authorizes
/// each forward when the tunnel opens.
export function PortMapPanel() {
  const { t } = useTranslation()
  const token = useAppStore((state) => state.token)
  const security = useAppStore((state) => state.security)
  const saveSecurity = useAppStore((state) => state.saveSecurity)
  const refreshSecurity = useAppStore((state) => state.refreshSecurity)
  const [mappings, setMappings] = useState<PortMapping[]>([])
  const [form, setForm] = useState<PortmapForm>(EMPTY_PORTMAP_FORM)
  const [loadError, setLoadError] = useState('')
  const [actionError, setActionError] = useState('')
  const [busy, setBusy] = useState(false)
  const [savingHost, setSavingHost] = useState(false)
  const [hostError, setHostError] = useState(false)
  const hostAllowed = portForwardEnabled(security)

  const refresh = useCallback(async () => {
    if (!token) return
    try {
      const result = await fetchPortMappings(token)
      setMappings(result.mappings ?? [])
      setLoadError('')
    } catch (caught) {
      setLoadError(errorCode(caught))
    }
  }, [token])

  useEffect(() => {
    if (!token) return
    // The first sample and the poll are both external subscriptions; queue the
    // initial one so this effect does not cascade a synchronous render.
    const initial = window.setTimeout(() => void refresh(), 0)
    // Bytes and listening state are live; a short poll keeps the lights and
    // counters honest without a second event channel.
    const timer = window.setInterval(() => void refresh(), 2000)
    return () => {
      window.clearTimeout(initial)
      window.clearInterval(timer)
    }
  }, [token, refresh])

  const addMapping = async () => {
    if (!token || busy) return
    const parsed = buildPortmapInput(form)
    if (!parsed.ok) {
      setActionError(parsed.error)
      return
    }
    setBusy(true)
    setActionError('')
    try {
      const result = await createPortMapping(token, parsed.input)
      setMappings((current) => [...current, result.mapping])
      // Keep the target host for the next add; clear both ports so a preset
      // click or a manual entry starts fresh.
      setForm((current) => ({
        ...current,
        listenPort: '',
        targetPort: '',
      }))
    } catch (caught) {
      setActionError(errorCode(caught))
    } finally {
      setBusy(false)
    }
  }

  const toggleMapping = async (mapping: PortMapping) => {
    if (!token || busy) return
    setBusy(true)
    setActionError('')
    try {
      const result = await patchPortMapping(token, mapping.id, {
        enabled: !mapping.enabled,
      })
      setMappings((current) =>
        current.map((item) =>
          item.id === mapping.id ? result.mapping : item,
        ),
      )
    } catch (caught) {
      setActionError(errorCode(caught))
    } finally {
      setBusy(false)
    }
  }

  const removeMapping = async (mapping: PortMapping) => {
    if (!token || busy) return
    setBusy(true)
    setActionError('')
    try {
      await deletePortMapping(token, mapping.id)
      setMappings((current) =>
        current.filter((item) => item.id !== mapping.id),
      )
    } catch (caught) {
      setActionError(errorCode(caught))
    } finally {
      setBusy(false)
    }
  }

  // The host-side opt-in. The panel keeps the other settings from the last
  // GET so this save only changes this flag.
  const toggleHost = async (next: boolean) => {
    if (!security || savingHost) return
    setSavingHost(true)
    setHostError(false)
    const saved = await saveSecurity(portForwardSaveValues(security, next))
    if (saved) {
      // The POST reply omits a few GET-only fields (the long-term code), so
      // reload the full object before the Security tab renders from it.
      await refreshSecurity()
    } else {
      setHostError(true)
    }
    setSavingHost(false)
  }

  const visibleError = actionError || loadError

  return (
    <div className="space-y-5">
      <div className="rounded-lg border border-line-soft bg-panel/30 px-3 py-2">
        <div className="flex items-center gap-3">
          <div className="min-w-0 flex-1">
            <div className="text-[12px] font-medium">
              {t('portmap.hostToggle')}
            </div>
          </div>
          <MappingSwitch
            enabled={hostAllowed}
            disabled={!security || savingHost}
            label={t('portmap.hostToggle')}
            onChange={(next) => void toggleHost(next)}
          />
        </div>
        {hostError ? (
          <p className="mt-1 text-[10px] text-red-500" role="alert">
            {t('portmap.hostSaveFailed')}
          </p>
        ) : null}
      </div>

      <div>
        <h3 className="flex items-center gap-2 text-sm font-medium">
          <ArrowLeftRight size={15} className="text-accent" />
          {t('portmap.title')}
        </h3>
        <p className="mt-1 text-[11px] leading-relaxed text-muted">
          {t('portmap.intro')}
        </p>
      </div>

      <div className="space-y-2">
        <div className="text-[11px] font-medium text-muted">
          {t('portmap.presets')}
        </div>
        <div className="flex flex-wrap gap-1.5">
          {PORTMAP_PRESETS.map((preset) => (
            <button
              key={preset.id}
              type="button"
              onClick={() =>
                setForm((current) => applyPortmapPreset(current, preset))
              }
              className="rounded-lg border border-line px-2.5 py-1 text-[11px] text-muted transition hover:border-accent/40 hover:bg-accent-soft hover:text-accent"
            >
              {t(`portmap.preset.${preset.id}`)}
              <span className="ml-1 opacity-60">{preset.port}</span>
            </button>
          ))}
        </div>
      </div>

      <div className="space-y-1.5">
        {mappings.length === 0 ? (
          <p className="rounded-lg border border-dashed border-line px-3 py-4 text-center text-[11px] text-muted">
            {t('portmap.empty')}
          </p>
        ) : (
          mappings.map((mapping) => {
            const status = mappingStatus(mapping)
            return (
              <div
                key={mapping.id}
                className="flex items-center gap-3 rounded-lg border border-line-soft bg-panel/30 px-3 py-2"
              >
                <StatusDot status={status} />
                <div className="min-w-0 flex-1">
                  <div className="flex flex-wrap items-center gap-x-2 gap-y-0.5 text-[12px]">
                    <span className="font-medium tabular-nums">
                      {mapping.listen_host}:{mapping.listen_port || '—'}
                    </span>
                    <span className="text-muted">→</span>
                    <span className="truncate text-muted">
                      {mapping.target_host}:{mapping.target_port}
                    </span>
                  </div>
                  <div className="mt-0.5 flex flex-wrap items-center gap-x-2 text-[10px] text-muted">
                    <span>
                      {t('portmap.statusLabel')}:
                      {' '}
                      {t(`portmap.status.${status}`)}
                    </span>
                    <span>
                      {t('portmap.traffic', {
                        in: formatPortmapBytes(mapping.bytes_in),
                        out: formatPortmapBytes(mapping.bytes_out),
                      })}
                    </span>
                  </div>
                  {mapping.last_error ? (
                    <div
                      className="mt-0.5 truncate text-[10px] text-red-500"
                      title={mapping.last_error}
                    >
                      {mapping.last_error}
                    </div>
                  ) : null}
                </div>
                <MappingSwitch
                  enabled={mapping.enabled}
                  label={t('portmap.toggle')}
                  onChange={() => void toggleMapping(mapping)}
                />
                <button
                  type="button"
                  aria-label={t('portmap.delete')}
                  title={t('portmap.delete')}
                  onClick={() => void removeMapping(mapping)}
                  className="grid h-7 w-7 shrink-0 place-items-center rounded-lg text-muted transition hover:bg-panel hover:text-red-500"
                >
                  <Trash2 size={13} />
                </button>
              </div>
            )
          })
        )}
      </div>

      <div className="rounded-lg border border-line-soft bg-panel/30 p-3">
        <div className="grid grid-cols-[minmax(0,5rem)_minmax(0,1fr)_minmax(0,5rem)_auto] items-end gap-2">
          <label className="min-w-0">
            <span className="mb-1 block text-[10px] text-muted">
              {t('portmap.localPort')}
            </span>
            <input
              inputMode="numeric"
              value={form.listenPort}
              placeholder={t('portmap.localPortPlaceholder')}
              onChange={(event) =>
                setForm((current) => ({
                  ...current,
                  listenPort: event.target.value,
                }))
              }
              className="w-full rounded-lg border border-line bg-surface px-2 py-1.5 text-[12px] tabular-nums outline-none focus:border-accent/60"
            />
          </label>
          <label className="min-w-0">
            <span className="mb-1 block text-[10px] text-muted">
              {t('portmap.target')}
            </span>
            <div className="flex items-center gap-1">
              <input
                value={form.targetHost}
                placeholder="127.0.0.1"
                onChange={(event) =>
                  setForm((current) => ({
                    ...current,
                    targetHost: event.target.value,
                  }))
                }
                className="min-w-0 flex-1 rounded-lg border border-line bg-surface px-2 py-1.5 text-[12px] outline-none focus:border-accent/60"
              />
              <span className="text-[12px] text-muted">:</span>
              <input
                inputMode="numeric"
                value={form.targetPort}
                placeholder="3389"
                onChange={(event) =>
                  setForm((current) => ({
                    ...current,
                    targetPort: event.target.value,
                  }))
                }
                className="w-16 shrink-0 rounded-lg border border-line bg-surface px-2 py-1.5 text-[12px] tabular-nums outline-none focus:border-accent/60"
              />
            </div>
          </label>
          <div className="col-span-1" />
          <button
            type="button"
            disabled={busy}
            onClick={() => void addMapping()}
            className="inline-flex items-center gap-1 rounded-lg bg-accent px-3 py-1.5 text-[12px] font-medium text-white transition hover:opacity-90 disabled:opacity-50"
          >
            <Plus size={13} />
            {t('portmap.add')}
          </button>
        </div>
        <p className="mt-2 text-[10px] leading-relaxed text-muted">
          {t('portmap.addHint')}
        </p>
        {visibleError ? (
          <p className="mt-1 text-[10px] text-red-500" role="alert">
            {t(portmapErrorKey(visibleError))}
          </p>
        ) : null}
      </div>
    </div>
  )
}
