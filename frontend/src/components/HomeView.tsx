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
  LayoutGrid,
  List,
  Monitor,
  MousePointer2,
  RefreshCw,
  SquarePen,
  Trash2,
  Wifi,
} from 'lucide-react'
import { useEffect, useState, type FormEvent, type ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { useAppStore, type RecentConnection } from '../store/app'
import { ServiceCard } from './ServiceCard'

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
  const preferredMode = useAppStore((state) => state.preferredMode)
  const setPreferredMode = useAppStore((state) => state.setPreferredMode)
  const historyView = useAppStore((state) => state.historyView)
  const setHistoryView = useAppStore((state) => state.setHistoryView)

  const [rememberCode, setRememberCode] = useState(true)
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

  /// Every manual connection becomes a tab in this window. The session's own
  /// pop-out button (or dragging a tab out) still opens a separate window.
  const startConnection = (
    targetHost: string,
    targetPort: number,
    targetToken: string,
    label?: string,
    saveToken = false,
  ) => {
    const requestAccess = targetToken.trim() === ''
    createSession(
      targetHost,
      targetPort,
      targetToken,
      label,
      saveToken,
      requestAccess,
      preferredMode,
    )
    if (requestAccess) {
      rememberRecent(
        targetHost,
        targetPort,
        label?.trim() || targetHost,
        targetToken,
        saveToken,
      )
    }
  }

  const submit = (event: FormEvent) => {
    event.preventDefault()
    const numericPort = Number(port)
    const targetHost = host.trim()
    const targetPort = numericPort
    if (targetHost === '') {
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
    if (localMode) {
      const lower = targetHost.toLowerCase()
      const servingPort = server?.port || Number(window.location.port)
      const servingHost = window.location.hostname.toLowerCase()
      const loopback = ['127.0.0.1', 'localhost', '::1', '0.0.0.0'].includes(
        lower,
      )
      if (targetPort === servingPort && (loopback || lower === servingHost)) {
        setError('self_connect')
        return
      }
    }
    setError(null)
    startConnection(
      targetHost,
      targetPort,
      code.trim(),
      targetHost,
      rememberCode,
    )
  }

  const controlHost = () => {
    if (!device || !authToken) return
    const targetHost = window.location.hostname || server?.host || '127.0.0.1'
    const targetPort =
      Number(window.location.port) || server?.port || device.port || 28418
    createSession(
      targetHost,
      targetPort,
      authToken,
      device.device_id,
      false,
      false,
      preferredMode,
    )
  }

  const prefillFromRecent = (recent: RecentConnection) => {
    setHost(recent.host)
    setPort(String(recent.port))
    setCode(recent.token ?? '')
    // Saving is on by default; a saved entry stays opted in when re-opened.
    setRememberCode(true)
    setError(null)
    document.getElementById('connect-code')?.focus()
  }

  /// A history entry with a saved code connects directly; one without a code
  /// only fills the form so the user can type the current code.
  const connectRecent = (recent: RecentConnection) => {
    if (recent.token) {
      startConnection(
        recent.host,
        recent.port,
        recent.token,
        recent.label,
        true,
      )
      return
    }
    prefillFromRecent(recent)
  }

  const requestAccess = () => {
    const targetHost = host.trim()
    const targetPort = Number(port)
    if (!targetHost || !Number.isFinite(targetPort) || targetPort <= 0) {
      setError('invalid_endpoint')
      return
    }
    setError(null)
    startConnection(targetHost, targetPort, '', targetHost)
  }

  const errorText = error
    ? String(t(`error.${error}`, { defaultValue: error }))
    : null

  return (
    <div className="mx-auto w-full max-w-6xl px-4 py-6 sm:px-6 sm:py-8">
      <header className="mb-6 flex flex-wrap items-end justify-between gap-4">
        <div className="min-w-0">
          <div className="text-[10px] font-medium uppercase tracking-[0.22em] text-accent">
            {t('home.eyebrow')}
          </div>
          <h1 className="mt-1 text-[26px] font-semibold tracking-tight sm:text-[30px]">
            {t('home.title')}
          </h1>
          <p className="mt-1.5 max-w-2xl text-[13px] leading-relaxed text-muted">
            {t('home.subtitle')}
          </p>
        </div>
        <div className="flex flex-wrap items-center gap-1.5">
          <span className="chip">
            {localMode ? t('home.thisDevice') : t('home.thisHost')}
          </span>
          {device ? (
            <span className={device.capabilities.screen_capture ? 'chip chip-on' : 'chip'}>
              <Wifi size={11} />
              {device.capabilities.screen_capture
                ? t('home.online')
                : t('home.offline')}
            </span>
          ) : null}
          {device ? (
            <span className="chip">
              {device.service} · v{device.version}
            </span>
          ) : null}
        </div>
      </header>

      <div className="grid gap-5 lg:grid-cols-12">
        <div className="flex min-w-0 flex-col gap-4 lg:col-span-5">
        <section className="card relative overflow-hidden p-5 sm:p-6">
          <div className="pointer-events-none absolute -right-24 -top-24 h-56 w-56 rounded-full bg-accent-soft blur-3xl" />
          <div className="relative">
            <div className="flex flex-wrap items-center gap-2">
              <span className="flex items-center gap-2 text-sm font-medium">
                <span className="h-2 w-2 rounded-full bg-accent" />
                {localMode ? t('home.thisDevice') : t('home.thisHost')}
                <span className="chip">
                  {localMode ? t('home.localBadge') : t('home.remoteBadge')}
                </span>
              </span>
            </div>

            {device ? (
              <>
                <div className="mt-5">
                  <div className="text-[10px] uppercase tracking-[0.18em] text-muted">
                    {t('home.computerName')}
                  </div>
                  <div className="mt-1.5 truncate text-[24px] font-semibold tracking-tight text-ink sm:text-[26px]">
                    {device.device_name || device.device_id}
                  </div>
                  <div className="mt-1 font-mono text-[11px] text-muted">
                    {device.host}:{device.port}
                  </div>
                </div>

                {device.code ? (
                  <div className="mt-4 rounded-xl border border-line-soft bg-panel/50 p-3">
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
                      <div className="flex-1 rounded-lg border border-line bg-surface px-3 py-2 font-mono text-sm tracking-[0.3em] text-ink">
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
                        title={
                          showDeviceCode ? t('home.hide') : t('home.show')
                        }
                        aria-label={
                          showDeviceCode ? t('home.hide') : t('home.show')
                        }
                      >
                        {showDeviceCode ? (
                          <EyeOff size={14} />
                        ) : (
                          <Eye size={14} />
                        )}
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
          <ServiceCard />
          <div className="rounded-xl border border-line bg-panel/30 px-4 py-3.5">
            <div className="flex items-center gap-2 text-[11px] font-medium uppercase tracking-[0.16em] text-muted">
              <Info size={12} className="text-accent/80" />
              {t('home.tipsTitle')}
            </div>
            <ul className="mt-2.5 space-y-2 text-[11px] leading-relaxed text-muted">
              <li className="flex gap-2">
                <span className="mt-[7px] h-1 w-1 shrink-0 rounded-full bg-accent/70" />
                {t('home.tipCode')}
              </li>
              <li className="flex gap-2">
                <span className="mt-[7px] h-1 w-1 shrink-0 rounded-full bg-accent/70" />
                {t('home.tipModes')}
              </li>
              <li className="flex gap-2">
                <span className="mt-[7px] h-1 w-1 shrink-0 rounded-full bg-accent/70" />
                {t('home.tipTools')}
              </li>
            </ul>
          </div>
        </div>

        <section className="card p-5 sm:p-6 lg:col-span-7">
          <h2 className="text-base font-semibold">{t('home.connectTitle')}</h2>
          <p className="mt-1 text-[12px] leading-relaxed text-muted">
            {t('home.connectSubtitle')}
          </p>

          {localMode ? (
            <div className="mt-4 rounded-xl border border-line-soft bg-panel/40 p-2.5">
              <div className="flex items-center gap-2 px-1">
                <span className="text-[10px] uppercase tracking-[0.16em] text-muted">
                  {t('home.nearby')}
                </span>
                <button
                  type="button"
                  onClick={() => void refreshDiscovery()}
                  className="ml-auto grid h-6 w-6 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink"
                  title={t('home.refresh')}
                  aria-label={t('home.refresh')}
                >
                  <RefreshCw size={12} />
                </button>
              </div>
              {nearby.length === 0 ? (
                <p className="px-1 pb-1 pt-1.5 text-[11px] text-muted">
                  {t('home.noNearby')}
                </p>
              ) : (
                <div className="mt-1.5 flex gap-1.5 overflow-x-auto pb-1">
                  {nearby.map((peer) => (
                    <button
                      key={peer.device_id}
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
                        }
                      }}
                      className="shrink-0 rounded-lg border border-line bg-surface px-2.5 py-2 text-left transition hover:border-accent/40 hover:bg-panel"
                    >
                      <div className="max-w-[11rem] truncate text-[12px] font-medium text-ink">
                        {peer.name || peer.device_id}
                      </div>
                      <div className="mt-0.5 max-w-[11rem] truncate font-mono text-[10px] text-muted">
                        {peer.host}:{peer.port}
                      </div>
                    </button>
                  ))}
                </div>
              )}
            </div>
          ) : null}

          <form onSubmit={submit} className="mt-4 space-y-4">
            <div className="grid gap-3 sm:grid-cols-[1fr_120px]">
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
                <span className="mt-1 block text-[10px] text-muted">
                  {t('home.hostHint')}
                </span>
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

            <div>
              <span className="mb-1.5 block text-[10px] uppercase tracking-[0.16em] text-muted">
                {t('home.sessionMode')}
              </span>
              <div className="flex gap-1 rounded-xl border border-line bg-panel/60 p-1">
                <button
                  type="button"
                  onClick={() => setPreferredMode('control')}
                  className={
                    'flex flex-1 items-center justify-center gap-1.5 rounded-lg px-3 py-1.5 text-[12px] transition ' +
                    (preferredMode === 'control'
                      ? 'bg-accent text-on-accent shadow-sm'
                      : 'text-muted hover:text-ink')
                  }
                >
                  <MousePointer2 size={13} />
                  {t('home.modeControl')}
                </button>
                <button
                  type="button"
                  onClick={() => setPreferredMode('view')}
                  className={
                    'flex flex-1 items-center justify-center gap-1.5 rounded-lg px-3 py-1.5 text-[12px] transition ' +
                    (preferredMode === 'view'
                      ? 'bg-accent text-on-accent shadow-sm'
                      : 'text-muted hover:text-ink')
                  }
                >
                  <Eye size={13} />
                  {t('home.modeView')}
                </button>
              </div>
              <span className="mt-1 block text-[10px] text-muted">
                {preferredMode === 'control'
                  ? t('home.modeControlHint')
                  : t('home.modeViewHint')}
              </span>
            </div>

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
              <button
                type="button"
                onClick={requestAccess}
                className="btn"
                title={t('home.requestAccessHint')}
              >
                {t('home.requestAccess')}
              </button>
            </div>
          </form>

          <div className="mt-4 flex items-start gap-2 rounded-lg bg-panel px-3 py-2.5 text-[11px] leading-relaxed text-muted">
            <Info size={13} className="mt-0.5 shrink-0 text-accent/80" />
            {t('home.idNote')}
          </div>
        </section>
      </div>

      <section className="card mt-5 p-5 sm:p-6">
        <div className="flex flex-wrap items-center gap-2">
          <History size={15} className="text-accent" />
          <span className="text-sm font-medium">{t('home.recent')}</span>
          {recents.length > 0 ? (
            <span className="chip">{recents.length}</span>
          ) : null}
          <div className="ml-auto flex items-center gap-1 rounded-lg border border-line bg-panel/60 p-0.5">
            <button
              type="button"
              onClick={() => setHistoryView('cards')}
              className={
                'grid h-7 w-7 place-items-center rounded-md transition ' +
                (historyView === 'cards'
                  ? 'bg-surface text-ink shadow-sm'
                  : 'text-muted hover:text-ink')
              }
              title={t('home.historyCards')}
              aria-label={t('home.historyCards')}
            >
              <LayoutGrid size={13} />
            </button>
            <button
              type="button"
              onClick={() => setHistoryView('list')}
              className={
                'grid h-7 w-7 place-items-center rounded-md transition ' +
                (historyView === 'list'
                  ? 'bg-surface text-ink shadow-sm'
                  : 'text-muted hover:text-ink')
              }
              title={t('home.historyList')}
              aria-label={t('home.historyList')}
            >
              <List size={13} />
            </button>
          </div>
        </div>

        {recents.length === 0 ? (
          <div className="mt-4 rounded-xl border border-dashed border-line px-4 py-9 text-center text-[12px] text-muted">
            {t('home.recentEmpty')}
          </div>
        ) : historyView === 'cards' ? (
          <div className="mt-4 grid gap-3 sm:grid-cols-2 xl:grid-cols-3">
            {recents.map((recent) => (
              <div
                key={`${recent.host}:${recent.port}:${recent.at}`}
                className="group rounded-xl border border-line bg-panel/40 p-3 transition hover:border-accent/40 hover:bg-panel"
              >
                <div className="flex items-start gap-2.5">
                  <div className="grid h-9 w-9 shrink-0 place-items-center rounded-lg bg-accent-soft text-accent">
                    <Monitor size={16} />
                  </div>
                  <div className="min-w-0 flex-1">
                    <div className="truncate text-[13px] font-medium text-ink">
                      {recent.label}
                    </div>
                    <div className="mt-0.5 truncate font-mono text-[10px] text-muted">
                      {recent.host}:{recent.port}
                    </div>
                  </div>
                  {recent.token ? (
                    <span className="chip chip-on shrink-0 !px-1.5 !py-0 text-[9px]">
                      {t('home.saved')}
                    </span>
                  ) : null}
                </div>
                <div className="mt-2.5 flex items-center gap-1.5">
                  <button
                    type="button"
                    onClick={() => connectRecent(recent)}
                    disabled={!recent.token}
                    className="btn btn-primary h-7 flex-1 px-2 text-[11px]"
                    title={
                      recent.token
                        ? t('home.connectSaved')
                        : t('home.prefillHint')
                    }
                  >
                    <Wifi size={12} />
                    {recent.token ? t('home.connect') : t('home.prefill')}
                  </button>
                  <button
                    type="button"
                    onClick={() => prefillFromRecent(recent)}
                    className="btn btn-quiet h-7 w-7 !px-0"
                    title={t('home.prefill')}
                    aria-label={t('home.prefill')}
                  >
                    <SquarePen size={12} />
                  </button>
                  <button
                    type="button"
                    onClick={() => forgetRecent(recent.at)}
                    className="btn btn-quiet h-7 w-7 !px-0 hover:!border-danger/50 hover:!text-danger"
                    title={t('home.forget')}
                    aria-label={t('home.forget')}
                  >
                    <Trash2 size={12} />
                  </button>
                </div>
                <div className="mt-2 text-[10px] text-muted/80">
                  {relativeTime(recent.at, i18n.language)}
                </div>
              </div>
            ))}
          </div>
        ) : (
          <ul className="mt-3 divide-y divide-line-soft">
            {recents.map((recent) => (
              <li
                key={`${recent.host}:${recent.port}:${recent.at}`}
                className="flex items-center gap-2 py-2.5"
              >
                <div className="grid h-8 w-8 shrink-0 place-items-center rounded-lg bg-panel text-muted">
                  <Monitor size={14} />
                </div>
                <button
                  type="button"
                  onClick={() => connectRecent(recent)}
                  className="min-w-0 flex-1 text-left"
                  title={
                    recent.token
                      ? t('home.connectSaved')
                      : t('home.prefillHint')
                  }
                >
                  <div className="flex items-center gap-1.5">
                    <span className="truncate text-[13px] text-ink">
                      {recent.label}
                    </span>
                    {recent.token ? (
                      <span className="chip chip-on !px-1.5 !py-0 text-[9px]">
                        {t('home.saved')}
                      </span>
                    ) : null}
                  </div>
                  <div className="mt-0.5 flex gap-2 font-mono text-[10px] text-muted">
                    <span>
                      {recent.host}:{recent.port}
                    </span>
                    <span>·</span>
                    <span>{relativeTime(recent.at, i18n.language)}</span>
                  </div>
                </button>
                <button
                  type="button"
                  onClick={() => prefillFromRecent(recent)}
                  className="grid h-7 w-7 place-items-center rounded-md text-muted/60 transition hover:bg-panel hover:text-ink"
                  aria-label={t('home.prefill')}
                  title={t('home.prefill')}
                >
                  <SquarePen size={13} />
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
  )
}
