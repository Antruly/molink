import {
  Check,
  Gauge,
  Languages,
  Moon,
  Network,
  SlidersHorizontal,
  Sparkles,
  Sun,
  Timer,
  Waves,
} from 'lucide-react'
import { useState, type ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { setLanguage } from '../i18n'
import type { ServerState } from '../lib/api'
import { useAppStore, type QualityMode, type Theme, type UiMode } from '../store/app'
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

export function PreferencesControls() {
  const { t, i18n } = useTranslation()
  const theme = useAppStore((state) => state.theme)
  const setTheme = useAppStore((state) => state.setTheme)
  const uiMode = useAppStore((state) => state.uiMode)
  const setUiMode = useAppStore((state) => state.setUiMode)
  const quality = useAppStore((state) => state.qualityWatermark)
  const setQuality = useAppStore((state) => state.setQualityWatermark)
  const qualityMode = useAppStore((state) => state.qualityMode)
  const setQualityMode = useAppStore((state) => state.setQualityMode)
  const lockTimeout = useAppStore((state) => state.lockTimeout)
  const setLockTimeout = useAppStore((state) => state.setLockTimeout)
  const server = useAppStore((state) => state.server)
  const token = useAppStore((state) => state.token)
  const setNetworkPorts = useAppStore((state) => state.setNetworkPorts)
  const language = i18n.language.startsWith('zh') ? 'zh-CN' : 'en'

  return (
    <div className="space-y-5">
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
          <Sparkles size={14} className="text-accent" />
          {t('ui.mode')}
        </div>
        <Segment<UiMode>
          value={uiMode}
          onChange={setUiMode}
          options={[
            {
              value: 'simple',
              label: t('ui.simple'),
              icon: <Sparkles size={13} />,
            },
            {
              value: 'advanced',
              label: t('ui.advanced'),
              icon: <SlidersHorizontal size={13} />,
            },
          ]}
        />
      </div>

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

      <div className="space-y-2">
        <div className="flex items-center gap-2 text-[12px] font-medium text-ink">
          <Gauge size={14} className="text-accent" />
          {t('settings.defaultQuality')}
        </div>
        <p className="text-[11px] leading-relaxed text-muted">
          {t('settings.qualityHint')}
        </p>
        <select
          value={qualityMode}
          onChange={(event) =>
            setQualityMode(event.target.value as QualityMode)
          }
          className="field"
        >
          <option value="turbo">{t('session.qualityTurbo')}</option>
          <option value="smooth">{t('session.qualitySmooth')}</option>
          <option value="balanced">{t('session.qualityBalanced')}</option>
          <option value="quality">{t('session.qualityHigh')}</option>
          <option value="auto">{t('session.qualityAuto')}</option>
        </select>
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
          onChange={(event) => void setLockTimeout(Number(event.target.value))}
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

      {server ? (
        <NetworkPortSettings
          key={`${server.tcp_port}:${server.udp_port}`}
          server={server}
          onSave={setNetworkPorts}
        />
      ) : null}

      {server?.local ? <ServicePanel token={token} /> : null}
      {server?.local ? <WindowsSessionsPanel token={token} /> : null}

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
    </div>
  )
}
