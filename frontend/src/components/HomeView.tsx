import {
  AlertTriangle,
  Check,
  Clipboard,
  Copy,
  Eye,
  EyeOff,
  History,
  Info,
  Keyboard,
  Monitor,
  RefreshCw,
  Trash2,
  Wifi,
} from 'lucide-react'
import { useEffect, useState, type FormEvent, type ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { claimFirstWindow, openSessionWindow } from '../lib/windows'
import { useAppStore, type RecentConnection } from '../store/app'

const params = new URLSearchParams(window.location.search)

function relativeTime(at: number, locale: string): string {
  const minutes = Math.round((at - Date.now()) / 60000)
  const formatter = new Intl.RelativeTimeFormat(locale, { numeric: 'auto' })
  if (Math.abs(minutes) < 60) return formatter.format(minutes, 'minute')
  const hours = Math.round(minutes / 60)
  if (Math.abs(hours) < 24) return formatter.format(hours, 'hour')
  return formatter.format(Math.round(hours / 24), 'day')
}

function Capability({
  on,
  icon,
  label,
}: {
  on: boolean
  icon: ReactNode
  label: string
}) {
  return (
    <span className={on ? 'chip chip-on' : 'chip'}>
      {icon}
      {label}
    </span>
  )
}

export function formatDeviceId(raw: string): string {
  const clean = raw
    .toUpperCase()
    .replace(/[^A-Z0-9]/g, '')
    .slice(0, 9)
  return [clean.slice(0, 3), clean.slice(3, 6), clean.slice(6, 9)]
    .filter(Boolean)
    .join('-')
}

export function HomeView() {
  const { t, i18n } = useTranslation()
  const device = useAppStore((state) => state.device)
  const localMode = useAppStore((state) => state.localMode)
  const server = useAppStore((state) => state.server)
  const authToken = useAppStore((state) => state.token)
  const stateError = useAppStore((state) => state.stateError)
  const refreshState = useAppStore((state) => state.refreshState)
  const createSession = useAppStore((state) => state.createSession)
  const setAuthCode = useAppStore((state) => state.setAuthCode)
  const recents = useAppStore((state) => state.recents)
  const forgetRecent = useAppStore((state) => state.forgetRecent)
  const nearby = useAppStore((state) => state.nearby)
  const refreshDiscovery = useAppStore((state) => state.refreshDiscovery)
  const refreshDevice = useAppStore((state) => state.refreshDevice)
  const rememberRecent = useAppStore((state) => state.rememberRecent)

  const [deviceIdInput, setDeviceIdInput] = useState('')
  const [rememberCode, setRememberCode] = useState(false)
  const [host, setHost] = useState('')
  const [port, setPort] = useState(params.get('port') ?? '28418')
  const [code, setCode] = useState('')
  const [showCode, setShowCode] = useState(false)
  const [showDeviceCode, setShowDeviceCode] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [copied, setCopied] = useState<string | null>(null)

  useEffect(() => {
    if (!localMode) return
    void refreshDiscovery()
    void refreshDevice()
    const timer = window.setInterval(() => {
      void refreshDiscovery()
      void refreshDevice()
    }, 3000)
    return () => window.clearInterval(timer)
  }, [localMode, refreshDiscovery, refreshDevice])

  const prefillFromNearby = (peer: {
    device_id: string
    name: string
    host: string
    port: number
  }) => {
    setDeviceIdInput(formatDeviceId(peer.device_id))
    setHost(peer.host)
    setPort(String(peer.port))
    setCode('')
    setError(null)
    document.getElementById('connect-code')?.focus()
  }

  const copy = async (key: string, value: string) => {
    try {
      await navigator.clipboard.writeText(value)
      setCopied(key)
      window.setTimeout(() => setCopied(null), 1500)
    } catch {
      setError('clipboard_unavailable')
    }
  }

  const startConnection = (
    targetHost: string,
    targetPort: number,
    targetToken: string,
    label?: string,
    saveToken = false,
  ) => {
    if (localMode && claimFirstWindow()) {
      if (openSessionWindow({
        host: targetHost,
        port: targetPort,
        token: targetToken,
        label,
      })) {
        rememberRecent(
          targetHost,
          targetPort,
          label?.trim() || targetHost,
          targetToken,
          saveToken,
        )
        return
      }
    }
    createSession(targetHost, targetPort, targetToken, label, saveToken)
  }

  const submit = (event: FormEvent) => {
    event.preventDefault()
    const numericPort = Number(port)
    let targetHost = host.trim()
    let targetPort = numericPort
    const rawDeviceId = deviceIdInput.replace(/-/g, '')
    if (rawDeviceId !== '') {
      if (rawDeviceId.length !== 9) {
        setError('device_not_found')
        return
      }
      const match = nearby.find(
        (peer) => formatDeviceId(peer.device_id) === deviceIdInput,
      )
      if (!match) {
        setError('device_not_found')
        return
      }
      targetHost = match.host
      targetPort = match.port
    } else if (targetHost === '') {
      setError('no_host')
      return
    }
    if (
      !Number.isInteger(targetPort) ||
      targetPort < 1 ||
      targetPort > 65535
    ) {
      setError('invalid_port')
      return
    }
    if (code.trim() === '') {
      setError('no_token')
      return
    }
    if (localMode) {
      const lower = targetHost.toLowerCase()
      const servingPort = server?.port || Number(window.location.port)
      const servingHost = window.location.hostname.toLowerCase()
      const loopback = ['127.0.0.1', 'localhost', '::1', '0.0.0.0'].includes(
        lower,
      )
      if (
        targetPort === servingPort &&
        (loopback || lower === servingHost)
      ) {
        setError('self_connect')
        return
      }
    }
    setError(null)
    startConnection(targetHost, targetPort, code.trim(), targetHost, rememberCode)
  }

  const controlHost = () => {
    if (!device || !authToken) return
    const host = window.location.hostname || server?.host || '127.0.0.1'
    const port =
      Number(window.location.port) || server?.port || device.port || 28418
    createSession(host, port, authToken, device.device_id)
  }

  const prefillFromRecent = (recent: RecentConnection) => {
    setHost(recent.host)
    setPort(String(recent.port))
    setCode(recent.token ?? '')
    setError(null)
    document.getElementById('connect-code')?.focus()
  }

  const errorText = error
    ? String(t(`error.${error}`, { defaultValue: error }))
    : null

  return (
    <div className="mx-auto w-full max-w-6xl px-4 py-7 sm:px-7 sm:py-9">
      <div className="mb-6">
        <h1 className="text-[26px] font-semibold tracking-tight sm:text-[30px]">
          {t('home.title')}
        </h1>
        <p className="mt-1.5 max-w-2xl text-sm leading-relaxed text-muted">
          {t('home.subtitle')}
        </p>
      </div>

      {localMode ? (
        <section className="card mb-5 p-5 sm:p-6">
          <div className="flex items-center justify-between gap-3">
            <div>
              <h2 className="text-base font-semibold">{t('home.nearby')}</h2>
              <p className="mt-1 text-[12px] text-muted">
                {t('home.nearbyHint')}
              </p>
            </div>
            <button
              type="button"
              onClick={() => void refreshDiscovery()}
              className="btn btn-quiet h-8 w-8 !px-0"
              title={t('home.refresh')}
              aria-label={t('home.refresh')}
            >
              <RefreshCw size={14} />
            </button>
          </div>
          {nearby.length === 0 ? (
            <p className="mt-4 text-[12px] text-muted">{t('home.noNearby')}</p>
          ) : (
            <ul className="mt-4 grid gap-2 sm:grid-cols-2 lg:grid-cols-3">
              {nearby.map((peer) => (
                <li key={peer.device_id}>
                  <button
                    type="button"
                    onClick={() => prefillFromNearby(peer)}
                    onDoubleClick={() => {
                      prefillFromNearby(peer)
                      const saved = recents.find(
                        (item) =>
                          item.host === peer.host &&
                          item.port === peer.port &&
                          item.token,
                      )
                      if (saved?.token) {
                        startConnection(
                          peer.host,
                          peer.port,
                          saved.token,
                          peer.name || peer.device_id,
                          true,
                        )
                      } else {
                        document.getElementById('connect-code')?.focus()
                      }
                    }}
                    className="w-full rounded-lg border border-line bg-panel px-3 py-2.5 text-left transition hover:border-accent/40 hover:bg-surface"
                  >
                    <div className="flex items-center justify-between gap-2">
                      <span className="truncate text-[13px] font-medium text-ink">
                        {peer.name || peer.device_id}
                      </span>
                      <span className="chip">{peer.port}</span>
                    </div>
                    <div className="mt-1 truncate font-mono text-[11px] text-muted">
                      {peer.device_id} · {peer.host}
                    </div>
                  </button>
                </li>
              ))}
            </ul>
          )}
        </section>
      ) : null}

      <div className="grid gap-5 lg:grid-cols-2">
        <section className="card relative overflow-hidden p-5 sm:p-6">
          <div className="pointer-events-none absolute -right-20 -top-24 h-56 w-56 rounded-full bg-accent-soft blur-3xl" />
          <div className="relative">
            <div className="flex flex-wrap items-center gap-2">
              <span className="flex items-center gap-2 text-sm font-medium">
                <span className="h-2 w-2 rounded-full bg-accent" />
                {localMode ? t('home.thisDevice') : t('home.thisHost')}
                <span className="chip">
                  {localMode ? t('home.localBadge') : t('home.remoteBadge')}
                </span>
              </span>
              {device ? (
                <>
                  <span className="chip">
                    {device.service} · v{device.version}
                  </span>
                  {device.capabilities.screen_capture ? (
                    <span className="chip chip-on">
                      <Wifi size={11} />
                      {t('home.online')}
                    </span>
                  ) : null}
                </>
              ) : null}
            </div>

            {device ? (
              <>
                <div className="mt-5">
                  <div className="text-[10px] uppercase tracking-[0.18em] text-muted">
                    {t('home.deviceId')}
                  </div>
                  <div className="mt-1.5 flex items-center gap-2">
                    <div className="font-mono text-[24px] tracking-[0.14em] text-ink sm:text-[26px]">
                      {device.device_id}
                    </div>
                    <button
                      type="button"
                      onClick={() => copy('id', device.device_id)}
                      className="btn btn-quiet h-8 w-8 !px-0"
                      title={t('home.copy')}
                      aria-label={t('home.copy')}
                    >
                      {copied === 'id' ? (
                        <Check size={14} className="text-accent" />
                      ) : (
                        <Copy size={14} />
                      )}
                    </button>
                  </div>
                </div>

                {device.code ? (
                  <div className="mt-4">
                    <div className="flex items-center gap-1.5 text-[10px] uppercase tracking-[0.18em] text-muted">
                      {t('home.verification')}
                      <span className="normal-case tracking-normal text-muted/70">
                        · {t('home.verificationHint')}
                      </span>
                      <span className="ml-auto normal-case tracking-normal text-muted/70">
                        {device.code.mode === 'single'
                          ? t('home.singleUse')
                          : t('home.multiRemaining', {
                              count: device.code.remaining,
                            })}
                      </span>
                    </div>
                    <div className="mt-1.5 flex items-center gap-2">
                      <div className="flex-1 rounded-lg border border-line bg-panel px-3 py-2 font-mono text-sm tracking-[0.3em] text-ink">
                        {showDeviceCode
                          ? device.code.value
                          : '•'.repeat(
                              Math.min(12, device.code.value.length),
                            )}
                      </div>
                      <button
                        type="button"
                        onClick={() => setShowDeviceCode((value) => !value)}
                        className="btn btn-quiet h-8 w-8 !px-0"
                        title={showDeviceCode ? t('home.hide') : t('home.show')}
                        aria-label={showDeviceCode ? t('home.hide') : t('home.show')}
                      >
                        {showDeviceCode ? <EyeOff size={14} /> : <Eye size={14} />}
                      </button>
                      <button
                        type="button"
                        onClick={() => copy('code', device.code!.value)}
                        className="btn btn-quiet h-8 w-8 !px-0"
                        title={t('home.copy')}
                        aria-label={t('home.copy')}
                      >
                        {copied === 'code' ? (
                          <Check size={14} className="text-accent" />
                        ) : (
                          <Copy size={14} />
                        )}
                      </button>
                    </div>
                    <div className="mt-2 flex flex-wrap items-center gap-1.5">
                      <button
                        type="button"
                        onClick={() => void setAuthCode('single', 1)}
                        className={
                          'btn h-7 px-2 text-[11px] ' +
                          (device.code.mode === 'single' ? 'btn-primary' : '')
                        }
                      >
                        {t('home.singleUse')}
                      </button>
                      <button
                        type="button"
                        onClick={() => void setAuthCode('multi', 20)}
                        className={
                          'btn h-7 px-2 text-[11px] ' +
                          (device.code.mode === 'multi' ? 'btn-primary' : '')
                        }
                      >
                        {t('home.multiUse')}
                      </button>
                      <button
                        type="button"
                        onClick={() =>
                          void setAuthCode(
                            device.code!.mode,
                            device.code!.uses,
                          )
                        }
                        className="btn btn-quiet h-7 w-7 !px-0"
                        title={t('home.refreshCode')}
                        aria-label={t('home.refreshCode')}
                      >
                        <RefreshCw size={12} />
                      </button>
                    </div>
                  </div>
                ) : null}

                <div className="mt-4 flex flex-wrap gap-1.5">
                  <Capability
                    on={device.capabilities.screen_capture}
                    icon={<Monitor size={11} />}
                    label={t('home.capCapture')}
                  />
                  <Capability
                    on={device.capabilities.input_injection}
                    icon={<Keyboard size={11} />}
                    label={t('home.capInput')}
                  />
                  <Capability
                    on={device.capabilities.clipboard}
                    icon={<Clipboard size={11} />}
                    label={t('home.capClipboard')}
                  />
                </div>

                <div className="mt-5 flex flex-wrap items-center gap-2">
                  {localMode ? (
                    <span className="chip chip-on">
                      <Wifi size={11} />
                      {t('home.localAccess')}
                    </span>
                  ) : (
                    <button
                      type="button"
                      onClick={controlHost}
                      className="btn btn-primary"
                    >
                      <Wifi size={14} />
                      {t('home.controlThisDevice')}
                    </button>
                  )}
                  <button
                    type="button"
                    onClick={() => void refreshState()}
                    className="btn btn-quiet h-9 w-9 !px-0"
                    title={t('home.refresh')}
                    aria-label={t('home.refresh')}
                  >
                    <RefreshCw size={14} />
                  </button>
                </div>
              </>
            ) : (
              <div className="mt-5 rounded-lg border border-warn/30 bg-warn/10 p-4 text-sm text-warn">
                <div className="flex items-center gap-2 font-medium">
                  <AlertTriangle size={15} />
                  {t('home.noDevice')}
                </div>
                <div className="mt-1 text-[12px] text-muted">
                  {stateError ?? t('home.noDeviceHint')}
                </div>
                <button
                  type="button"
                  onClick={() => void refreshState()}
                  className="btn mt-3"
                >
                  <RefreshCw size={13} />
                  {t('home.retry')}
                </button>
              </div>
            )}
          </div>
        </section>

        <section className="card p-5 sm:p-6">
          <h2 className="text-base font-semibold">{t('home.connectTitle')}</h2>
          <p className="mt-1 text-[12px] leading-relaxed text-muted">
            {t('home.connectSubtitle')}
          </p>

          <form onSubmit={submit} className="mt-5 space-y-4">
            <label className="block">
              <span className="mb-1.5 block text-[10px] uppercase tracking-[0.16em] text-muted">
                {t('home.deviceId')}
              </span>
              <input
                value={deviceIdInput}
                onChange={(event) =>
                  setDeviceIdInput(formatDeviceId(event.target.value))
                }
                className="field font-mono"
                placeholder="ABC-DEF-GHJ"
                maxLength={11}
                spellCheck={false}
                autoComplete="off"
              />
              <span className="mt-1 block text-[10px] text-muted">
                {t('home.deviceIdHint')}
              </span>
            </label>
            <div className="grid grid-cols-[1fr_110px] gap-3">
              <label className="block">
                <span className="mb-1.5 block text-[10px] uppercase tracking-[0.16em] text-muted">
                  {t('home.host')}
                </span>
                <input
                  value={host}
                  onChange={(event) => setHost(event.target.value)}
                  className="field font-mono"
                  placeholder="192.168.1.20"
                  spellCheck={false}
                  autoComplete="off"
                />
              </label>
              <label className="block">
                <span className="mb-1.5 block text-[10px] uppercase tracking-[0.16em] text-muted">
                  {t('home.port')}
                </span>
                <input
                  value={port}
                  onChange={(event) => setPort(event.target.value)}
                  className="field font-mono"
                  inputMode="numeric"
                  autoComplete="off"
                />
              </label>
            </div>

            <label className="block">
              <span className="mb-1.5 block text-[10px] uppercase tracking-[0.16em] text-muted">
                {t('home.code')}
              </span>
              <div className="relative">
                <input
                  id="connect-code"
                  value={code}
                  onChange={(event) => setCode(event.target.value)}
                  type={showCode ? 'text' : 'password'}
                  className="field pr-10 font-mono"
                  autoComplete="off"
                  spellCheck={false}
                />
                <button
                  type="button"
                  onClick={() => setShowCode((value) => !value)}
                  className="absolute inset-y-0 right-0 grid w-10 place-items-center text-muted transition hover:text-ink"
                  aria-label={showCode ? t('home.hide') : t('home.show')}
                >
                  {showCode ? <EyeOff size={14} /> : <Eye size={14} />}
                </button>
              </div>
            </label>

            <label className="flex items-center gap-2 text-[11px] text-muted">
              <input
                type="checkbox"
                checked={rememberCode}
                onChange={(event) => setRememberCode(event.target.checked)}
                className="h-3.5 w-3.5"
              />
              {t('home.rememberCode')}
            </label>

            {errorText ? (
              <div className="rounded-lg border border-danger/35 bg-danger/10 px-3 py-2 text-[12px] text-danger">
                {errorText}
              </div>
            ) : null}

            <div className="flex flex-wrap items-center gap-2">
              <button type="submit" className="btn btn-primary">
                {t('home.connect')}
              </button>
            </div>
          </form>

          <div className="mt-4 flex items-start gap-2 rounded-lg bg-panel px-3 py-2.5 text-[11px] leading-relaxed text-muted">
            <Info size={13} className="mt-0.5 shrink-0 text-accent/80" />
            {t('home.idNote')}
          </div>
        </section>
      </div>

      <div className="mt-5 grid gap-5 lg:grid-cols-2">
        <section className="card p-5 sm:p-6">
          <div className="flex items-center gap-2 text-sm font-medium">
            <History size={15} className="text-accent" />
            {t('home.recent')}
          </div>
          {recents.length === 0 ? (
            <div className="mt-4 rounded-lg border border-dashed border-line px-4 py-7 text-center text-[12px] text-muted">
              {t('home.recentEmpty')}
            </div>
          ) : (
            <ul className="mt-3 divide-y divide-line-soft">
              {recents.map((recent) => (
                <li
                  key={`${recent.host}:${recent.port}:${recent.at}`}
                  className="flex items-center gap-2 py-2.5"
                >
                  <button
                    type="button"
                    onClick={() => prefillFromRecent(recent)}
                    className="min-w-0 flex-1 text-left"
                  >
                    <div className="truncate text-[13px] text-ink">
                      {recent.label}
                    </div>
                    <div className="mt-0.5 flex gap-2 font-mono text-[11px] text-muted">
                      <span>
                        {recent.host}:{recent.port}
                      </span>
                      <span>·</span>
                      <span>{relativeTime(recent.at, i18n.language)}</span>
                    </div>
                  </button>
                  <button
                    type="button"
                    onClick={() => forgetRecent(recent.at)}
                    className="grid h-7 w-7 place-items-center rounded-md text-muted/60 transition hover:bg-danger/10 hover:text-danger"
                    aria-label={t('home.forget')}
                    title={t('home.forget')}
                  >
                    <Trash2 size={13} />
                  </button>
                </li>
              ))}
            </ul>
          )}
        </section>
      </div>
    </div>
  )
}
