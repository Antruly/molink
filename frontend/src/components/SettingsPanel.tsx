import { Eye, EyeOff, RefreshCw, ShieldCheck, X } from 'lucide-react'
import { useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import type { AuthCode, SecuritySettings } from '../lib/api'
import { useAppStore } from '../store/app'
import { PreferencesControls } from './PreferencesControls'

function Switch({
  checked,
  disabled,
  onChange,
  label,
}: {
  checked: boolean
  disabled?: boolean
  onChange: (checked: boolean) => void
  label: string
}) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      aria-label={label}
      disabled={disabled}
      onClick={() => onChange(!checked)}
      className={
        'relative h-6 w-11 shrink-0 rounded-full border transition ' +
        (checked ? 'border-accent/50 bg-accent/80' : 'border-line bg-panel') +
        (disabled ? ' opacity-40' : '')
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

function SecurityControls({
  settings,
  onSave,
  onLongTerm,
  onCustomCode,
}: {
  settings: SecuritySettings
  onSave: (values: {
    allowRemote: boolean
    allowWeb: boolean
    discovery: boolean
    name: string
  }) => Promise<boolean>
  onLongTerm: (value: string) => Promise<boolean>
  onCustomCode: (uses: number, value: string) => Promise<AuthCode | null>
}) {
  const { t } = useTranslation()
  const [allowRemote, setAllowRemote] = useState(settings.allow_remote_access)
  const [allowWeb, setAllowWeb] = useState(settings.allow_web_client)
  const [discovery, setDiscovery] = useState(settings.discovery_enabled)
  const [name, setName] = useState(settings.device_name)
  const [longTerm, setLongTerm] = useState('')
  const [showLongTerm, setShowLongTerm] = useState(false)
  const [customUses, setCustomUses] = useState('20')
  const [customValue, setCustomValue] = useState('')
  const [note, setNote] = useState<string | null>(null)

  const save = async () => {
    const ok = await onSave({
      allowRemote,
      allowWeb,
      discovery: allowRemote ? discovery : false,
      name,
    })
    setNote(ok ? t('settings.saved') : t('error.settings_failed'))
  }

  const applyLongTerm = async (value: string) => {
    const ok = await onLongTerm(value)
    setNote(ok ? t('settings.saved') : t('error.settings_failed'))
    if (ok) setLongTerm('')
  }

  const applyCustomCode = async () => {
    const uses = Number(customUses)
    if (!Number.isInteger(uses) || uses < 1) {
      setNote(t('error.invalid_uses'))
      return
    }
    const code = await onCustomCode(uses, customValue.trim())
    setNote(code ? t('settings.saved') : t('error.settings_failed'))
    if (code) setCustomValue('')
  }

  return (
    <div className="mt-6 space-y-4 border-t border-line-soft pt-4">
      <div className="flex items-center gap-2 text-[12px] font-medium text-ink">
        <ShieldCheck size={14} className="text-accent" />
        {t('settings.security')}
      </div>

      <div className="space-y-3 rounded-lg border border-line bg-panel/60 p-3">
        <div className="flex items-start justify-between gap-3">
          <div>
            <div className="text-[12px] text-ink">{t('settings.allowRemote')}</div>
            <div className="mt-0.5 text-[11px] text-muted">
              {t('settings.allowRemoteHint')}
            </div>
          </div>
          <Switch
            checked={allowRemote}
            onChange={setAllowRemote}
            label={t('settings.allowRemote')}
          />
        </div>
        <div className="flex items-start justify-between gap-3">
          <div>
            <div className="text-[12px] text-ink">{t('settings.allowWeb')}</div>
            <div className="mt-0.5 text-[11px] text-muted">
              {t('settings.allowWebHint')}
            </div>
          </div>
          <Switch
            checked={allowWeb}
            onChange={setAllowWeb}
            label={t('settings.allowWeb')}
          />
        </div>
        <div className="flex items-start justify-between gap-3">
          <div>
            <div className="text-[12px] text-ink">{t('settings.discovery')}</div>
            <div className="mt-0.5 text-[11px] text-muted">
              {t('settings.discoveryHint')}
            </div>
          </div>
          <Switch
            checked={discovery}
            disabled={!allowRemote}
            onChange={setDiscovery}
            label={t('settings.discovery')}
          />
        </div>
        <label className="block">
          <span className="mb-1 block text-[10px] uppercase tracking-[0.14em] text-muted">
            {t('settings.deviceName')}
          </span>
          <input
            value={name}
            onChange={(event) => setName(event.target.value)}
            className="field"
          />
        </label>
        <div className="flex items-center gap-2">
          <button type="button" onClick={() => void save()} className="btn">
            {t('settings.apply')}
          </button>
          {settings.restart_required ? (
            <span className="text-[11px] text-warn">
              {t('settings.restartRequired')}
            </span>
          ) : note ? (
            <span className="text-[11px] text-muted">{note}</span>
          ) : null}
        </div>
      </div>

      <div className="space-y-2 rounded-lg border border-line bg-panel/60 p-3">
        <div className="text-[12px] text-ink">{t('settings.longTermCode')}</div>
        <div className="text-[11px] text-muted">{t('settings.longTermHint')}</div>
        <div className="flex items-center gap-2">
          <div className="flex-1 truncate rounded-md border border-line bg-surface px-2 py-1.5 font-mono text-[12px]">
            {showLongTerm ? settings.long_term_code : '••••••••'}
          </div>
          <button
            type="button"
            onClick={() => setShowLongTerm((value) => !value)}
            className="btn btn-quiet h-8 w-8 !px-0"
            title={showLongTerm ? t('home.hide') : t('home.show')}
            aria-label={showLongTerm ? t('home.hide') : t('home.show')}
          >
            {showLongTerm ? <EyeOff size={14} /> : <Eye size={14} />}
          </button>
          <button
            type="button"
            onClick={() => void applyLongTerm('')}
            className="btn btn-quiet h-8 w-8 !px-0"
            title={t('settings.generate')}
            aria-label={t('settings.generate')}
          >
            <RefreshCw size={14} />
          </button>
        </div>
        <div className="flex items-center gap-2">
          <input
            value={longTerm}
            onChange={(event) => setLongTerm(event.target.value)}
            placeholder={t('settings.newLongTerm')}
            className="field font-mono"
          />
          <button
            type="button"
            onClick={() => void applyLongTerm(longTerm)}
            disabled={longTerm.trim() === ''}
            className="btn"
          >
            {t('settings.apply')}
          </button>
        </div>
      </div>

      <div className="space-y-2 rounded-lg border border-line bg-panel/60 p-3">
        <div className="text-[12px] text-ink">{t('settings.customMulti')}</div>
        <div className="text-[11px] text-muted">
          {t('settings.customMultiHint')}
        </div>
        <div className="grid grid-cols-[1fr_90px_auto] gap-2">
          <input
            value={customValue}
            onChange={(event) => setCustomValue(event.target.value)}
            placeholder="123456"
            className="field font-mono"
          />
          <input
            value={customUses}
            onChange={(event) => setCustomUses(event.target.value)}
            inputMode="numeric"
            className="field font-mono"
            aria-label={t('settings.uses')}
          />
          <button type="button" onClick={() => void applyCustomCode()} className="btn">
            {t('settings.apply')}
          </button>
        </div>
      </div>
    </div>
  )
}

export function SettingsPanel() {
  const { t } = useTranslation()
  const open = useAppStore((state) => state.settingsOpen)
  const setOpen = useAppStore((state) => state.setSettingsOpen)
  const device = useAppStore((state) => state.device)
  const localMode = useAppStore((state) => state.localMode)
  const security = useAppStore((state) => state.security)
  const refreshSecurity = useAppStore((state) => state.refreshSecurity)
  const saveSecurity = useAppStore((state) => state.saveSecurity)
  const setLongTermCode = useAppStore((state) => state.setLongTermCode)
  const setAuthCode = useAppStore((state) => state.setAuthCode)

  useEffect(() => {
    if (!open) return
    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape') setOpen(false)
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [open, setOpen])

  useEffect(() => {
    if (open && localMode) void refreshSecurity()
  }, [open, localMode, refreshSecurity])

  if (!open) return null

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-black/35 p-4 backdrop-blur-sm"
      onMouseDown={(event) => {
        if (event.target === event.currentTarget) setOpen(false)
      }}
    >
      <div className="max-h-[90vh] w-full max-w-md overflow-y-auto rounded-2xl border border-line bg-surface p-5 shadow-2xl">
        <div className="mb-5 flex items-center justify-between">
          <h2 className="text-base font-semibold">{t('settings.title')}</h2>
          <button
            type="button"
            onClick={() => setOpen(false)}
            aria-label={t('settings.close')}
            className="grid h-8 w-8 place-items-center rounded-lg text-muted transition hover:bg-panel hover:text-ink"
          >
            <X size={16} />
          </button>
        </div>

        <PreferencesControls />

        {localMode && security ? (
          <SecurityControls
            key={`${security.allow_remote_access}:${security.allow_web_client}:${security.discovery_enabled}:${security.device_name}:${security.long_term_code}`}
            settings={security}
            onSave={saveSecurity}
            onLongTerm={setLongTermCode}
            onCustomCode={(uses, value) => setAuthCode('multi', uses, value)}
          />
        ) : null}

        <div className="mt-6 border-t border-line-soft pt-4 text-[11px] leading-relaxed text-muted">
          <div className="flex justify-between">
            <span>{t('settings.version')}</span>
            <span className="font-mono">{device?.version ?? '0.3.0'}</span>
          </div>
          <div className="mt-1 flex justify-between">
            <span>{t('settings.protocol')}</span>
            <span className="font-mono">v1</span>
          </div>
          <div className="mt-1 flex justify-between">
            <span>{t('settings.license')}</span>
            <span>MIT</span>
          </div>
        </div>
      </div>
    </div>
  )
}
