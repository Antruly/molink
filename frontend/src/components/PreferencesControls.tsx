import { Languages, Moon, Sun, Timer, Waves } from 'lucide-react'
import type { ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { setLanguage } from '../i18n'
import { useAppStore, type Theme } from '../store/app'

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

export function PreferencesControls() {
  const { t, i18n } = useTranslation()
  const theme = useAppStore((state) => state.theme)
  const setTheme = useAppStore((state) => state.setTheme)
  const quality = useAppStore((state) => state.qualityWatermark)
  const setQuality = useAppStore((state) => state.setQualityWatermark)
  const lockTimeout = useAppStore((state) => state.lockTimeout)
  const setLockTimeout = useAppStore((state) => state.setLockTimeout)
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
