import {
  Activity,
  Check,
  Cpu,
  Gauge,
  Languages,
  Moon,
  Network,
  Sun,
  Timer,
  Volume2,
  Waves,
} from 'lucide-react'
import { useEffect, useState, type ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { setLanguage } from '../i18n'
import {
  fetchAv1Settings,
  postAv1Settings,
  type Av1Settings,
  type ServerState,
} from '../lib/api'
import { FPS_CHOICES, parseFpsSetting } from '../lib/frame-rate'
import { useAppStore, type QualityMode, type Theme } from '../store/app'
import { ServicePanel } from './ServicePanel'
import { WindowsSessionsPanel } from './WindowsSessionsPanel'

function Segment<T extends string>({
  value,
  options,
  onChange,
}: {
  value: T
  options: { value: T; label: string; icon?: ReactNode }[]
  onChange: (value: T) => void
}) {
  return (
    <div className="flex rounded-lg border border-line bg-panel p-1">
      {options.map((option) => (
        <button
          key={option.value}
          type="button"
          onClick={() => onChange(option.value)}
          className={
            'flex flex-1 items-center justify-center gap-1.5 rounded-md px-3 py-1.5 text-[12px] transition ' +
            (value === option.value
              ? 'bg-surface text-accent shadow-sm'
              : 'text-muted hover:text-ink')
          }
        >
          {option.icon}
          {option.label}
        </button>
      ))}
    </div>
  )
}

function Toggle({
  checked,
  onChange,
  label,
}: {
  checked: boolean
  onChange: (checked: boolean) => void
  label: string
}) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      aria-label={label}
      onClick={() => onChange(!checked)}
      className={
        'relative h-6 w-11 shrink-0 rounded-full border transition ' +
        (checked
          ? 'border-accent/50 bg-accent/80'
          : 'border-line bg-panel')
      }
    >
      <span
        className={
          'absolute top-0.5 rounded-full bg-surface shadow transition-all ' +
          (checked ? 'left-[22px]' : 'left-0.5')
        }
        style={{ width: '1.125rem', height: '1.125rem' }}
      />
    </button>
  )
}

function NetworkPortSettings({
  server,
  onSave,
}: {
  server: ServerState
  onSave: (tcp: number, udp: number) => Promise<boolean>
}) {
  const { t } = useTranslation()
  const [tcpPort, setTcpPort] = useState(String(server.tcp_port))
  const [udpPort, setUdpPort] = useState(String(server.udp_port))
  const [note, setNote] = useState<string | null>(null)

  const save = async () => {
    const tcp = Number(tcpPort)
    const udp = Number(udpPort)
    if (
      !Number.isInteger(tcp) ||
      tcp < 1 ||
      tcp > 65535 ||
      !Number.isInteger(udp) ||
      udp < 1 ||
      udp > 65535
    ) {
      setNote(t('error.invalid_port'))
      return
    }
    const saved = await onSave(tcp, udp)
    setNote(saved ? null : t('error.settings_failed'))
  }

  return (
    <div className="space-y-2">
      <div className="flex items-center gap-2 text-[12px] font-medium text-ink">
        <Network size={14} className="text-accent" />
        {t('settings.network')}
      </div>
      <div className="grid grid-cols-2 gap-2">
        <label className="block">
          <span className="mb-1 block text-[10px] uppercase tracking-[0.14em] text-muted">
            {t('settings.tcpPort')}
          </span>
          <input
            value={tcpPort}
            onChange={(event) => setTcpPort(event.target.value)}
            inputMode="numeric"
            className="field font-mono"
          />
        </label>
        <label className="block">
          <span className="mb-1 block text-[10px] uppercase tracking-[0.14em] text-muted">
            {t('settings.udpPort')}
          </span>
          <input
            value={udpPort}
            onChange={(event) => setUdpPort(event.target.value)}
            inputMode="numeric"
            className="field font-mono"
          />
        </label>
      </div>
      <div className="flex items-center gap-2">
        <button type="button" onClick={() => void save()} className="btn">
          <Check size={13} />
          {t('settings.savePorts')}
        </button>
        {server.restart_required ? (
          <span className="text-[11px] text-warn">
            {t('settings.restartRequired')}
          </span>
        ) : note ? (
          <span className="text-[11px] text-muted">{note}</span>
        ) : null}
      </div>
      <p className="text-[11px] leading-relaxed text-muted">
        {t('settings.networkHint')}
      </p>
    </div>
  )
}

const QUALITY_PRESETS: {
  value: QualityMode
  label: string
  description: string
}[] = [
  {
    value: 'auto',
    label: 'session.qualityAuto',
    description: 'settings.qualityDescAuto',
  },
  {
    value: 'turbo',
    label: 'session.qualityTurbo',
    description: 'settings.qualityDescTurbo',
  },
  {
    value: 'smooth',
    label: 'session.qualitySmooth',
    description: 'settings.qualityDescSmooth',
  },
  {
    value: 'balanced',
    label: 'session.qualityBalanced',
    description: 'settings.qualityDescBalanced',
  },
  {
    value: 'quality',
    label: 'session.qualityHigh',
    description: 'settings.qualityDescQuality',
  },
  {
    value: 'ultra',
    label: 'session.qualityUltra',
    description: 'settings.qualityDescUltra',
  },
  {
    value: 'lossless',
    label: 'session.qualityLossless',
    description: 'settings.qualityDescLossless',
  },
  {
    value: 'game',
    label: 'session.qualityGame',
    description: 'settings.qualityDescGame',
  },
]

const QUALITY_LABEL_KEYS: Record<string, string> = {
  turbo: 'session.qualityTurbo',
  smooth: 'session.qualitySmooth',
  balanced: 'session.qualityBalanced',
  quality: 'session.qualityHigh',
  raw: 'session.qualityLossless',
  auto: 'session.qualityAuto',
}

const AV1_SPEEDS: { value: Av1Settings['speed']; label: string }[] = [
  { value: 'auto', label: 'settings.av1SpeedAuto' },
  { value: 'fast', label: 'settings.av1SpeedFast' },
  { value: 'balanced', label: 'settings.av1SpeedBalanced' },
  { value: 'quality', label: 'settings.av1SpeedQuality' },
]

/// Host-side AV1 encoder preferences. The encoder runs on the machine that
/// produces the picture, so this panel is only rendered for the local core.
function Av1Controls({ token }: { token: string }) {
  const { t } = useTranslation()
  const [data, setData] = useState<Av1Settings | null>(null)
  const [enabled, setEnabled] = useState(true)
  const [fullFrame, setFullFrame] = useState(true)
  const [device, setDevice] = useState('auto')
  const [speed, setSpeed] = useState<Av1Settings['speed']>('auto')
  const [autoParams, setAutoParams] = useState(true)
  const [qp, setQp] = useState(24)
  const [maxArea, setMaxArea] = useState(35)
  const [threads, setThreads] = useState(4)
  const [keyframeInterval, setKeyframeInterval] = useState(0)
  const [busy, setBusy] = useState(false)
  const [note, setNote] = useState<'saved' | 'failed' | null>(null)

  const apply = (next: Av1Settings) => {
    setData(next)
    setEnabled(next.enabled)
    setFullFrame(next.full_frame ?? true)
    setDevice(next.device)
    setSpeed(next.speed)
    const automatic =
      next.qp === 0 && next.max_area_percent === 0 && next.threads === 0
    setAutoParams(automatic)
    setQp(next.effective?.qp ?? 24)
    setMaxArea(next.effective?.max_area_percent ?? 35)
    setThreads(next.effective?.threads ?? 4)
    setKeyframeInterval(next.keyframe_interval_seconds ?? 0)
  }

  useEffect(() => {
    if (!token) return
    let cancelled = false
    fetchAv1Settings(token)
      .then((next) => {
        if (!cancelled) apply(next)
      })
      .catch(() => undefined)
    return () => {
      cancelled = true
    }
  }, [token])

  const save = async () => {
    setBusy(true)
    setNote(null)
    try {
      const next = await postAv1Settings(token, {
        enabled,
        full_frame: fullFrame,
        device,
        speed,
        qp: autoParams ? 0 : qp,
        max_area_percent: autoParams ? 0 : maxArea,
        threads: autoParams ? 0 : threads,
        keyframe_interval_seconds: keyframeInterval,
      })
      apply(next)
      setNote('saved')
    } catch {
      setNote('failed')
    } finally {
      setBusy(false)
    }
  }

  if (!data) return null

  const keyframeOptions = [0, 1, 2, 3, 5, 10, 15, 30, 60]
  if (!keyframeOptions.includes(keyframeInterval)) {
    keyframeOptions.push(keyframeInterval)
    keyframeOptions.sort((a, b) => a - b)
  }

  return (
    <div className="space-y-3 border-t border-line-soft pt-4">
      <div className="space-y-2">
        <div className="flex items-center gap-2 text-[12px] font-medium text-ink">
          <Timer size={14} className="text-accent" />
          {t('settings.keyframeInterval')}
        </div>
        <select
          value={keyframeInterval}
          onChange={(event) =>
            setKeyframeInterval(Number(event.target.value))
          }
          className="field"
        >
          {keyframeOptions.map((seconds) => (
            <option key={seconds} value={seconds}>
              {seconds === 0
                ? t('settings.keyframeOff')
                : t('settings.secondsValue', { count: seconds })}
            </option>
          ))}
        </select>
        <p className="text-[11px] leading-relaxed text-muted">
          {t('settings.keyframeIntervalHint')}
        </p>
      </div>
      <div className="border-t border-line-soft" />
      <div className="flex items-center gap-2 text-[12px] font-medium text-ink">
        <Cpu size={14} className="text-accent" />
        {t('settings.av1Title')}
        <span
          className={
            'ml-auto rounded-full px-2 py-0.5 text-[10px] font-normal ' +
            (data.available
              ? 'bg-accent-soft text-accent'
              : 'bg-panel text-muted')
          }
        >
          {data.available
            ? t('settings.av1Available')
            : t('settings.av1UnavailableBadge')}
        </span>
      </div>
      <p className="text-[11px] leading-relaxed text-muted">
        {t('settings.av1Hint')}
      </p>

      <div className="flex items-center justify-between gap-4">
        <div>
          <div className="text-[12px] font-medium text-ink">
            {t('settings.av1Enable')}
          </div>
          <div className="mt-1 text-[11px] leading-relaxed text-muted">
            {t('settings.av1EnableHint')}
          </div>
        </div>
        <Toggle
          checked={enabled}
          onChange={setEnabled}
          label={t('settings.av1Enable')}
        />
      </div>

      <div className="flex items-center justify-between gap-4">
        <div>
          <div className="text-[12px] font-medium text-ink">
            {t('settings.av1FullFrame')}
          </div>
          <div className="mt-1 text-[11px] leading-relaxed text-muted">
            {t('settings.av1FullFrameHint')}
          </div>
        </div>
        <Toggle
          checked={fullFrame}
          onChange={setFullFrame}
          label={t('settings.av1FullFrame')}
        />
      </div>

      <label className="block">
        <span className="mb-1 block text-[10px] uppercase tracking-[0.14em] text-muted">
          {t('settings.av1Device')}
        </span>
        <select
          value={device}
          onChange={(event) => setDevice(event.target.value)}
          className="field"
          disabled={!data.available}
        >
          <option value="auto">{t('settings.av1DeviceAuto')}</option>
          {data.devices.map((item) => (
            <option key={item.id} value={item.id} disabled={!item.available}>
              {item.name}
              {item.hardware ? ` · ${t('settings.av1Gpu')}` : ''}
            </option>
          ))}
          {device !== 'auto' &&
          !data.devices.some((item) => item.id === device) ? (
            <option value={device}>{device}</option>
          ) : null}
        </select>
      </label>
      <p className="text-[11px] leading-relaxed text-muted">
        {!data.available
          ? t('settings.av1BuildMissing')
          : data.devices.length <= 1
            ? t('settings.av1DeviceCpuOnly')
            : t('settings.av1DeviceHint')}
      </p>

      <label className="block">
        <span className="mb-1 block text-[10px] uppercase tracking-[0.14em] text-muted">
          {t('settings.av1Speed')}
        </span>
        <select
          value={speed}
          onChange={(event) =>
            setSpeed(event.target.value as Av1Settings['speed'])
          }
          className="field"
          disabled={!data.available}
        >
          {AV1_SPEEDS.map((item) => (
            <option key={item.value} value={item.value}>
              {t(item.label)}
            </option>
          ))}
        </select>
      </label>
      <p className="text-[11px] leading-relaxed text-muted">
        {t('settings.av1SpeedHint')}
      </p>

      <div className="flex items-center justify-between gap-4">
        <div>
          <div className="text-[12px] font-medium text-ink">
            {t('settings.av1AutoParams')}
          </div>
          <div className="mt-1 text-[11px] leading-relaxed text-muted">
            {t('settings.av1AutoParamsHint')}
          </div>
        </div>
        <Toggle
          checked={autoParams}
          onChange={setAutoParams}
          label={t('settings.av1AutoParams')}
        />
      </div>

      {!autoParams ? (
        <div className="space-y-3">
          <label className="block">
            <span className="flex items-center justify-between text-[11px] text-ink">
              <span>{t('settings.av1Qp')}</span>
              <span className="font-mono text-accent">{qp}</span>
            </span>
            <input
              type="range"
              min={1}
              max={63}
              value={qp}
              onChange={(event) => setQp(Number(event.target.value))}
              className="mt-1 w-full"
            />
            <span className="text-[10px] leading-relaxed text-muted">
              {t('settings.av1QpHint')}
            </span>
          </label>
          <label className={fullFrame ? 'hidden' : 'block'}>
            <span className="flex items-center justify-between text-[11px] text-ink">
              <span>{t('settings.av1MaxArea')}</span>
              <span className="font-mono text-accent">{maxArea}%</span>
            </span>
            <input
              type="range"
              min={5}
              max={100}
              step={5}
              value={maxArea}
              onChange={(event) => setMaxArea(Number(event.target.value))}
              className="mt-1 w-full"
            />
            <span className="text-[10px] leading-relaxed text-muted">
              {t('settings.av1MaxAreaHint')}
            </span>
          </label>
          <label className="block">
            <span className="flex items-center justify-between text-[11px] text-ink">
              <span>{t('settings.av1Threads')}</span>
              <span className="font-mono text-accent">{threads}</span>
            </span>
            <input
              type="range"
              min={1}
              max={32}
              value={threads}
              onChange={(event) => setThreads(Number(event.target.value))}
              className="mt-1 w-full"
            />
            <span className="text-[10px] leading-relaxed text-muted">
              {t('settings.av1ThreadsHint')}
            </span>
          </label>
        </div>
      ) : null}

      {data.effective ? (
        <p className="rounded-lg bg-panel/60 px-3 py-2 text-[11px] leading-relaxed text-muted">
          {data.effective.hardware
            ? t('settings.av1EffectiveGpu', {
                quality: data.active_quality
                  ? t(
                      QUALITY_LABEL_KEYS[data.active_quality] ??
                        'session.qualityBalanced',
                    )
                  : '—',
                preset: data.effective.enc_mode,
                qp: data.effective.qp,
                area: data.effective.max_area_percent,
              })
            : t('settings.av1Effective', {
                quality: data.active_quality
                  ? t(
                      QUALITY_LABEL_KEYS[data.active_quality] ??
                        'session.qualityBalanced',
                    )
                  : '—',
                qp: data.effective.qp,
                area: data.effective.max_area_percent,
                mode: data.effective.enc_mode,
                threads: data.effective.threads,
              })}
        </p>
      ) : null}

      <div className="flex items-center gap-2">
        <button
          type="button"
          className="btn btn-primary h-8 px-3 text-[11px]"
          disabled={busy}
          onClick={() => void save()}
        >
          <Check size={12} />
          {t('settings.av1Save')}
        </button>
        {note === 'saved' ? (
          <span className="text-[11px] text-accent">{t('settings.av1Saved')}</span>
        ) : note === 'failed' ? (
          <span className="text-[11px] text-warn">{t('error.settings_failed')}</span>
        ) : null}
      </div>
    </div>
  )
}

export function PreferencesControls({
  section = 'all',
}: {
  section?: 'general' | 'display' | 'network' | 'service' | 'all'
}) {
  const { t, i18n } = useTranslation()
  const theme = useAppStore((state) => state.theme)
  const setTheme = useAppStore((state) => state.setTheme)
  const quality = useAppStore((state) => state.qualityWatermark)
  const setQuality = useAppStore((state) => state.setQualityWatermark)
  const autoAudio = useAppStore((state) => state.autoAudio)
  const setAutoAudio = useAppStore((state) => state.setAutoAudio)
  const qualityMode = useAppStore((state) => state.qualityMode)
  const setQualityMode = useAppStore((state) => state.setQualityMode)
  const frameRate = useAppStore((state) => state.frameRate)
  const setFrameRate = useAppStore((state) => state.setFrameRate)
  const lockTimeout = useAppStore((state) => state.lockTimeout)
  const setLockTimeout = useAppStore((state) => state.setLockTimeout)
  const server = useAppStore((state) => state.server)
  const token = useAppStore((state) => state.token)
  const setNetworkPorts = useAppStore((state) => state.setNetworkPorts)
  const language = i18n.language.startsWith('zh') ? 'zh-CN' : 'en'
  const showGeneral = section === 'general' || section === 'all'
  const showDisplay = section === 'display' || section === 'all'
  const showNetwork = section === 'network' || section === 'all'
  const showService = section === 'service' || section === 'all'

  return (
    <div className="space-y-5">
      {showGeneral ? (
        <>
          <div className="space-y-2">
            <div className="flex items-center gap-2 text-[12px] font-medium text-ink">
              <Sun size={14} className="text-accent" />
              {t('settings.theme')}
            </div>
            <Segment<Theme>
              value={theme}
              onChange={setTheme}
              options={[
                {
                  value: 'light',
                  label: t('settings.themeLight'),
                  icon: <Sun size={13} />,
                },
                {
                  value: 'dark',
                  label: t('settings.themeDark'),
                  icon: <Moon size={13} />,
                },
              ]}
            />
          </div>

          <div className="space-y-2">
            <div className="flex items-center gap-2 text-[12px] font-medium text-ink">
              <Timer size={14} className="text-accent" />
              {t('settings.autoLock')}
            </div>
            <p className="text-[11px] leading-relaxed text-muted">
              {t('settings.autoLockHint')}
            </p>
            <select
              value={lockTimeout}
              onChange={(event) =>
                void setLockTimeout(Number(event.target.value))
              }
              className="field"
            >
              {(lockTimeout === 0 ||
              [30, 60, 120, 300, 900].includes(lockTimeout)
                ? [30, 60, 120, 300, 900, 0]
                : [30, 60, 120, 300, 900, lockTimeout, 0]
              ).map((seconds) => (
                <option key={seconds} value={seconds}>
                  {seconds === 0
                    ? t('settings.never')
                    : seconds < 60
                      ? t('settings.secondsValue', { count: seconds })
                      : t('settings.minutesValue', {
                          count: Math.round(seconds / 60),
                        })}
                </option>
              ))}
            </select>
          </div>

          <div className="space-y-2">
            <div className="flex items-center gap-2 text-[12px] font-medium text-ink">
              <Languages size={14} className="text-accent" />
              {t('settings.language')}
            </div>
            <Segment<string>
              value={language}
              onChange={(value) => setLanguage(value)}
              options={[
                { value: 'en', label: 'English' },
                { value: 'zh-CN', label: '中文' },
              ]}
            />
          </div>
        </>
      ) : null}

      {showDisplay ? (
        <>
          <div className="flex items-center justify-between gap-4">
            <div>
              <div className="flex items-center gap-2 text-[12px] font-medium text-ink">
                <Waves size={14} className="text-accent" />
                {t('settings.watermark')}
              </div>
              <div className="mt-1 text-[11px] leading-relaxed text-muted">
                {t('settings.watermarkHint')}
              </div>
            </div>
            <Toggle
              checked={quality}
              onChange={setQuality}
              label={t('settings.watermark')}
            />
          </div>

          <div className="flex items-center justify-between gap-4">
            <div>
              <div className="flex items-center gap-2 text-[12px] font-medium text-ink">
                <Volume2 size={14} className="text-accent" />
                {t('settings.autoAudio')}
              </div>
              <div className="mt-1 text-[11px] leading-relaxed text-muted">
                {t('settings.autoAudioHint')}
              </div>
            </div>
            <Toggle
              checked={autoAudio}
              onChange={setAutoAudio}
              label={t('settings.autoAudio')}
            />
          </div>

          <div className="space-y-2">
            <div className="flex items-center gap-2 text-[12px] font-medium text-ink">
              <Activity size={14} className="text-accent" />
              {t('settings.frameRate')}
            </div>
            <p className="text-[11px] leading-relaxed text-muted">
              {t('settings.frameRateHint')}
            </p>
            <select
              value={String(frameRate)}
              onChange={(event) =>
                setFrameRate(parseFpsSetting(event.target.value))
              }
              className="field"
            >
              {FPS_CHOICES.map((choice) => (
                <option key={String(choice)} value={String(choice)}>
                  {choice === 'auto'
                    ? t('settings.frameRateAuto')
                    : t('settings.frameRateValue', { count: choice })}
                </option>
              ))}
            </select>
          </div>

          <div className="space-y-2">
            <div className="flex items-center gap-2 text-[12px] font-medium text-ink">
              <Gauge size={14} className="text-accent" />
              {t('settings.defaultQuality')}
            </div>
            <p className="text-[11px] leading-relaxed text-muted">
              {t('settings.qualityHint')}
            </p>
            <div className="space-y-1.5" role="radiogroup">
              {QUALITY_PRESETS.map((preset) => {
                const selected = qualityMode === preset.value
                return (
                  <button
                    key={preset.value}
                    type="button"
                    role="radio"
                    aria-checked={selected}
                    onClick={() => setQualityMode(preset.value)}
                    className={
                      'flex w-full items-start gap-2 rounded-lg border px-3 py-2 text-left transition ' +
                      (selected
                        ? 'border-accent/40 bg-accent-soft/60'
                        : 'border-line bg-panel/40 hover:border-line hover:bg-panel')
                    }
                  >
                    <span
                      className={
                        'mt-0.5 grid h-4 w-4 shrink-0 place-items-center rounded-full border ' +
                        (selected
                          ? 'border-accent bg-accent text-white'
                          : 'border-line text-transparent')
                      }
                    >
                      <Check size={10} />
                    </span>
                    <span className="min-w-0">
                      <span
                        className={
                          'block text-[12px] font-medium ' +
                          (selected ? 'text-accent' : 'text-ink')
                        }
                      >
                        {t(preset.label)}
                      </span>
                      <span className="mt-0.5 block text-[11px] leading-relaxed text-muted">
                        {t(preset.description)}
                      </span>
                    </span>
                  </button>
                )
              })}
            </div>
          </div>

          {server?.local && token ? <Av1Controls token={token} /> : null}
        </>
      ) : null}

      {showNetwork && server ? (
        <NetworkPortSettings
          key={`${server.tcp_port}:${server.udp_port}`}
          server={server}
          onSave={setNetworkPorts}
        />
      ) : null}

      {showService ? (
        <>
          <ServicePanel token={token} showUnavailable />
          {server?.local ? <WindowsSessionsPanel token={token} /> : null}
        </>
      ) : null}
    </div>
  )
}
