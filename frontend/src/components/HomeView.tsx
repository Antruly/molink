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
import { useState, type FormEvent, type ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { claimFirstWindow, openSessionWindow } from '../lib/windows'
import { useAppStore, type RecentConnection } from '../store/app'
import { PreferencesControls } from './PreferencesControls'

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

export function HomeView() {
  const { t, i18n } = useTranslation()
  const device = useAppStore((state) => state.device)
  const deviceError = useAppStore((state) => state.deviceError)
  const deviceLoading = useAppStore((state) => state.deviceLoading)
  const refreshDevice = useAppStore((state) => state.refreshDevice)
  const createSession = useAppStore((state) => state.createSession)
  const recents = useAppStore((state) => state.recents)
  const forgetRecent = useAppStore((state) => state.forgetRecent)

  const [host, setHost] = useState(params.get('host') ?? '127.0.0.1')
  const [port, setPort] = useState(params.get('port') ?? '21118')
  const [token, setToken] = useState('')
  const [showCode, setShowCode] = useState(false)
  const [showDeviceCode, setShowDeviceCode] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [copied, setCopied] = useState<string | null>(null)

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
  ) => {
    if (claimFirstWindow()) {
      if (openSessionWindow({
        host: targetHost,
        port: targetPort,
        token: targetToken,
        label,
      })) {
        return
      }
    }
    createSession(targetHost, targetPort, targetToken, label)
  }

  const submit = (event: FormEvent) => {
    event.preventDefault()
    const numericPort = Number(port)
    if (host.trim() === '') {
      setError('no_host')
      return
    }
    if (!Number.isInteger(numericPort) || numericPort < 1 || numericPort > 65535) {
      setError('invalid_port')
      return
    }
    if (token.trim() === '') {
      setError('no_token')
      return
    }
    setError(null)
    startConnection(host.trim(), numericPort, token.trim(), host.trim())
  }

  const connectLocal = () => {
    if (!device) return
    startConnection(
      device.host,
      device.port,
      device.token,
      t('home.thisDevice'),
    )
  }

  const prefillFromRecent = (recent: RecentConnection) => {
    setHost(recent.host)
    setPort(String(recent.port))
    setToken('')
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

      <div className="grid gap-5 lg:grid-cols-2">
        <section className="card relative overflow-hidden p-5 sm:p-6">
          <div className="pointer-events-none absolute -right-20 -top-24 h-56 w-56 rounded-full bg-accent-soft blur-3xl" />
          <div className="relative">
            <div className="flex flex-wrap items-center gap-2">
              <span className="flex items-center gap-2 text-sm font-medium">
                <span className="h-2 w-2 rounded-full bg-accent" />
                {t('home.thisDevice')}
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

                <div className="mt-4">
                  <div className="flex items-center gap-1.5 text-[10px] uppercase tracking-[0.18em] text-muted">
                    {t('home.verification')}
                    <span className="normal-case tracking-normal text-muted/70">
                      · {t('home.verificationHint')}
                    </span>
                  </div>
                  <div className="mt-1.5 flex items-center gap-2">
                    <div className="flex-1 rounded-lg border border-line bg-panel px-3 py-2 font-mono text-sm tracking-[0.3em] text-ink">
                      {showDeviceCode
                        ? device.token
                        : '•'.repeat(Math.min(12, device.token.length))}
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
                      onClick={() => copy('code', device.token)}
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
                </div>

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

                <div className="mt-5 flex items-center gap-2">
                  <button
                    type="button"
                    onClick={connectLocal}
                    className="btn btn-primary"
                  >
                    <Wifi size={14} />
                    {t('home.connectLocal')}
                  </button>
                  <button
                    type="button"
                    onClick={() => void refreshDevice()}
                    className="btn btn-quiet h-9 w-9 !px-0"
                    title={t('home.refresh')}
                    aria-label={t('home.refresh')}
                  >
                    <RefreshCw
                      size={14}
                      className={deviceLoading ? 'animate-spin' : ''}
                    />
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
                  {deviceError ?? t('home.noDeviceHint')}
                </div>
                <button
                  type="button"
                  onClick={() => void refreshDevice()}
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
            <div className="grid grid-cols-[1fr_110px] gap-3">
              <label className="block">
                <span className="mb-1.5 block text-[10px] uppercase tracking-[0.16em] text-muted">
                  {t('home.host')}
                </span>
                <input
                  value={host}
                  onChange={(event) => setHost(event.target.value)}
                  className="field font-mono"
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
                  value={token}
                  onChange={(event) => setToken(event.target.value)}
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

            {errorText ? (
              <div className="rounded-lg border border-danger/35 bg-danger/10 px-3 py-2 text-[12px] text-danger">
                {errorText}
              </div>
            ) : null}

            <div className="flex flex-wrap items-center gap-2">
              <button type="submit" className="btn btn-primary">
                {t('home.connect')}
              </button>
              <button
                type="button"
                onClick={() => {
                  if (!device) return
                  setHost(device.host)
                  setPort(String(device.port))
                  setToken(device.token)
                  setError(null)
                }}
                className="btn btn-quiet"
                disabled={!device}
              >
                {t('home.useLocalCode')}
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

        <section className="card p-5 sm:p-6">
          <h2 className="mb-4 text-sm font-medium">{t('settings.title')}</h2>
          <PreferencesControls />
        </section>
      </div>
    </div>
  )
}
