import {
  Copy,
  Eye,
  EyeOff,
  Info,
  Monitor,
  Network,
  RefreshCw,
  Server,
  Settings2,
  ShieldCheck,
  X,
} from 'lucide-react'
import { useCallback, useEffect, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import {
  fetchServerConfig,
  postServerConfig,
  type AuthCode,
  type SecuritySettings,
  type ServerConfig,
} from '../lib/api'
import { useAppStore } from '../store/app'
import { AboutPanel } from './AboutPanel'
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
  code,
  onCodePolicy,
}: {
  settings: SecuritySettings
  onSave: (values: {
    allowRemote: boolean
    allowWeb: boolean
    discovery: boolean
    allowAccessRequests: boolean
    encryptRemote: boolean
    encryptLan: boolean
    name: string
    stunServer?: string
  }) => Promise<boolean>
  onLongTerm: (value: string) => Promise<boolean>
  onCustomCode: (uses: number, value: string) => Promise<AuthCode | null>
  code: AuthCode | null
  onCodePolicy: (single: boolean, multi: boolean) => Promise<AuthCode | null>
}) {
  const { t } = useTranslation()
  const [allowRemote, setAllowRemote] = useState(settings.allow_remote_access)
  const [allowWeb, setAllowWeb] = useState(settings.allow_web_client)
  const [discovery, setDiscovery] = useState(settings.discovery_enabled)
  const [allowAccessRequests, setAllowAccessRequests] = useState(
    settings.allow_access_requests,
  )
  const [encryptRemote, setEncryptRemote] = useState(
    settings.encrypt_remote !== false,
  )
  const [encryptLan, setEncryptLan] = useState(
    settings.encrypt_lan === true,
  )
  const [name, setName] = useState(settings.device_name)
  const [stunServer, setStunServer] = useState(settings.stun_server ?? '')
  const [longTerm, setLongTerm] = useState('')
  const [showLongTerm, setShowLongTerm] = useState(false)
  const [customUses, setCustomUses] = useState('1')
  const [singleEnabled, setSingleEnabled] = useState(
    code?.single_enabled !== false,
  )
  const [multiEnabled, setMultiEnabled] = useState(
    code?.multi_enabled !== false,
  )
  const applyPolicy = async (single: boolean, multi: boolean) => {
    const result = await onCodePolicy(single, multi)
    if (result) {
      setSingleEnabled(result.single_enabled !== false)
      setMultiEnabled(result.multi_enabled !== false)
      setNote(t('settings.saved'))
    } else {
      setNote(t('error.settings_failed'))
    }
  }
  const [customValue, setCustomValue] = useState('')
  const [note, setNote] = useState<string | null>(null)

  const persist = async (values: {
    allowRemote?: boolean
    allowWeb?: boolean
    discovery?: boolean
    allowAccessRequests?: boolean
    encryptRemote?: boolean
    encryptLan?: boolean
    name?: string
    stunServer?: string
  }) => {
    const nextRemote = values.allowRemote ?? allowRemote
    const ok = await onSave({
      allowRemote: nextRemote,
      allowWeb: values.allowWeb ?? allowWeb,
      discovery: nextRemote ? (values.discovery ?? discovery) : false,
      allowAccessRequests:
        values.allowAccessRequests ?? allowAccessRequests,
      encryptRemote: values.encryptRemote ?? encryptRemote,
      encryptLan: values.encryptLan ?? encryptLan,
      name: values.name ?? settings.device_name,
      stunServer: values.stunServer ?? stunServer,
    })
    setNote(ok ? t('settings.saved') : t('error.settings_failed'))
    return ok
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
    <div className="space-y-4">
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
            onChange={(value) => {
              setAllowRemote(value)
              if (!value) setDiscovery(false)
              void persist({
                allowRemote: value,
                discovery: value ? discovery : false,
              })
            }}
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
            onChange={(value) => {
              setAllowWeb(value)
              void persist({ allowWeb: value })
            }}
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
            onChange={(value) => {
              setDiscovery(value)
              void persist({ discovery: value })
            }}
            label={t('settings.discovery')}
          />
        </div>
        <div className="flex items-start justify-between gap-3">
          <div>
            <div className="text-[12px] text-ink">
              {t('settings.allowAccessRequests')}
            </div>
            <div className="mt-0.5 text-[11px] text-muted">
              {t('settings.allowAccessRequestsHint')}
            </div>
          </div>
          <Switch
            checked={allowAccessRequests}
            onChange={(value) => {
              setAllowAccessRequests(value)
              void persist({ allowAccessRequests: value })
            }}
            label={t('settings.allowAccessRequests')}
          />
        </div>
        <div className="flex items-start justify-between gap-3">
          <div>
            <div className="text-[12px] text-ink">
              {t('settings.encryptRemote')}
            </div>
            <div className="mt-0.5 text-[11px] text-muted">
              {t('settings.encryptRemoteHint')}
            </div>
          </div>
          <Switch
            checked={encryptRemote}
            onChange={(value) => {
              setEncryptRemote(value)
              void persist({ encryptRemote: value })
            }}
            label={t('settings.encryptRemote')}
          />
        </div>
        <div className="flex items-start justify-between gap-3">
          <div>
            <div className="text-[12px] text-ink">
              {t('settings.encryptLan')}
            </div>
            <div className="mt-0.5 text-[11px] text-muted">
              {t('settings.encryptLanHint')}
            </div>
          </div>
          <Switch
            checked={encryptLan}
            onChange={(value) => {
              setEncryptLan(value)
              void persist({ encryptLan: value })
            }}
            label={t('settings.encryptLan')}
          />
        </div>
        <label className="block">
          <span className="mb-1 block text-[10px] uppercase tracking-[0.14em] text-muted">
            {t('settings.stunServer')}
          </span>
          <input
            className="field h-8 w-full px-2 font-mono text-[11px]"
            value={stunServer}
            onChange={(event) => setStunServer(event.target.value)}
            onBlur={() => void persist({ stunServer })}
            onKeyDown={(event) => {
              if (event.key === 'Enter') {
                event.preventDefault()
                void persist({ stunServer })
              }
            }}
            placeholder="stun.miwifi.com:3478"
            spellCheck={false}
          />
          <span className="mt-1 block text-[10px] leading-relaxed text-muted">
            {t('settings.stunServerHint')}
          </span>
        </label>
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
        <div className="text-[10px] leading-relaxed text-muted">
          {t('settings.autoSaveHint')}
        </div>
        <div className="flex items-center gap-2">
          <button
            type="button"
            onClick={() => void persist({ name })}
            className="btn"
          >
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
        <div className="text-[12px] text-ink">{t('settings.codePolicy')}</div>
        <label
          className={
            'flex items-center gap-2 text-[11px] ' +
            (settings.long_term_code ? 'text-muted' : 'text-muted opacity-50')
          }
        >
          <input
            type="checkbox"
            checked={singleEnabled}
            disabled={!settings.long_term_code}
            onChange={(event) =>
              void applyPolicy(event.target.checked, multiEnabled)
            }
          />
          {t('settings.singleCodeEnabled')}
        </label>
        <div className="text-[10px] leading-relaxed text-muted">
          {t('settings.singleCodeNeedsLongTerm')}
        </div>
        <label className="flex items-center gap-2 text-[11px] text-muted">
          <input
            type="checkbox"
            checked={multiEnabled}
            onChange={(event) =>
              void applyPolicy(singleEnabled, event.target.checked)
            }
          />
          {t('settings.multiCodeEnabled')}
        </label>
        <div className="text-[10px] leading-relaxed text-muted">
          {t('settings.multiCodeRotationHint')}
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


function ServerControls() {
  const { t } = useTranslation()
  const token = useAppStore((state) => state.token)
  const setServerEnabled = useAppStore((state) => state.setServerEnabled)
  const setDomain = useAppStore((state) => state.setDomain)
  const [url, setUrl] = useState('')
  const [invite, setInvite] = useState('')
  const [ca, setCa] = useState('')
  const [status, setStatus] = useState<ServerConfig | null>(null)
  const [note, setNote] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
  const [serverBusy, setServerBusy] = useState(false)
  const [domainInput, setDomainInput] = useState('')
  const [domainBusy, setDomainBusy] = useState(false)
  const [copied, setCopied] = useState(false)
  const initialized = useRef(false)

  const load = useCallback(async () => {
    if (!token) return
    try {
      const next = await fetchServerConfig(token)
      setStatus(next)
      if (!initialized.current) {
        // The built-in default is not a custom URL; showing it here made a
        // plain save pin the default as if the user had typed it.
        setUrl(next.custom_url ?? next.url)
        setInvite(next.invite)
        setCa(next.ca)
        initialized.current = true
      }
    } catch {
      // The core may still be booting.
    }
  }, [token])

  useEffect(() => {
    const initial = window.setTimeout(() => void load(), 0)
    const timer = window.setInterval(() => void load(), 2000)
    return () => {
      window.clearTimeout(initial)
      window.clearInterval(timer)
    }
  }, [load])

  const save = async (nextUrl: string) => {
    if (!token) return
    setBusy(true)
    setNote(null)
    try {
      const next = await postServerConfig(
        token,
        nextUrl.trim(),
        invite.trim(),
        ca.trim(),
      )
      setStatus(next)
      setUrl(next.custom_url ?? next.url)
      setNote(t('settings.saved'))
    } catch {
      setNote(t('error.settings_failed'))
    } finally {
      setBusy(false)
    }
  }

  const copyId = async () => {
    const value = status?.display_id || status?.device_id || ''
    if (!value) return
    try {
      await navigator.clipboard.writeText(value)
      setCopied(true)
      window.setTimeout(() => setCopied(false), 1500)
    } catch {
      // Clipboard access can be denied; the ID is still visible.
    }
  }

  const enabled = status?.enabled ?? true
  const registered = status?.registered ?? false
  const domainCode = status?.domain ?? ''
  const domainValid = status?.domain_valid ?? false
  const domainShown =
    domainCode.length === 9
      ? `${domainCode.slice(0, 3)}-${domainCode.slice(3, 6)}-${domainCode.slice(6)}`
      : domainCode

  const toggleServer = async (next: boolean) => {
    if (serverBusy) return
    setServerBusy(true)
    const ok = await setServerEnabled(next)
    setServerBusy(false)
    setNote(ok ? t('settings.saved') : t('error.settings_failed'))
    if (ok) void load()
  }

  const copyDomain = async () => {
    if (!domainCode) return
    try {
      await navigator.clipboard.writeText(domainCode)
      setNote(t('settings.domainCopied'))
    } catch {
      // Clipboard access can be denied; the code is still visible.
    }
  }

  const rotateDomain = async () => {
    if (domainBusy || !registered) return
    if (!window.confirm(t('settings.domainRotateConfirm'))) return
    setDomainBusy(true)
    const result = await setDomain('rotate')
    setDomainBusy(false)
    setNote(
      result === 'ok'
        ? t('settings.domainRotated')
        : t('error.settings_failed'),
    )
    if (result === 'ok') void load()
  }

  const joinDomain = async () => {
    if (domainBusy || !registered) return
    const value = domainInput.trim()
    if (!value) return
    setDomainBusy(true)
    const result = await setDomain('join', value)
    setDomainBusy(false)
    if (result === 'ok') {
      setDomainInput('')
      setNote(t('settings.domainJoined'))
      void load()
    } else if (result === 'unknown_domain') {
      setNote(t('settings.domainUnknown'))
    } else {
      setNote(t('error.settings_failed'))
    }
  }

  return (
    <div className="mt-2 space-y-3 border-t border-line-soft pt-4">
      <div className="flex items-center gap-2 text-[12px] font-medium text-ink">
        <Server size={14} className="text-accent" />
        {t('settings.server')}
      </div>
      <p className="text-[11px] leading-relaxed text-muted">
        {t('settings.serverHint')}
      </p>
      <label className="block">
        <span className="mb-1 block text-[10px] uppercase tracking-[0.16em] text-muted">
          {t('settings.serverUrl')}
        </span>
        <input
          value={url}
          onChange={(event) => setUrl(event.target.value)}
          className="field font-mono text-[12px]"
          placeholder={status?.default_url || 'https://molink.ssvr.top:28430'}
          spellCheck={false}
          autoComplete="off"
        />
        {status?.using_default ? (
          <span className="mt-1 block text-[10px] text-muted">
            {t('settings.serverUsingDefault', { url: status.default_url })}
          </span>
        ) : null}
      </label>
      <label className="block">
        <span className="mb-1 block text-[10px] uppercase tracking-[0.16em] text-muted">
          {t('settings.serverInvite')}
        </span>
        <input
          value={invite}
          onChange={(event) => setInvite(event.target.value)}
          className="field font-mono text-[12px]"
          placeholder={t('settings.serverInviteHint')}
          spellCheck={false}
          autoComplete="off"
        />
      </label>
      <label className="block">
        <span className="mb-1 block text-[10px] uppercase tracking-[0.16em] text-muted">
          {t('settings.serverCa')}
        </span>
        <input
          value={ca}
          onChange={(event) => setCa(event.target.value)}
          className="field font-mono text-[12px]"
          placeholder="/etc/ssl/private/molink-ca.pem"
          spellCheck={false}
          autoComplete="off"
        />
        <span className="mt-1 block text-[10px] text-muted">
          {t('settings.serverCaHint')}
        </span>
      </label>
      <div className="flex flex-wrap gap-2">
        <button
          type="button"
          disabled={busy}
          onClick={() => void save(url)}
          className="btn btn-primary h-8 px-3 text-[11px]"
        >
          {t('settings.serverSave')}
        </button>
        <button
          type="button"
          disabled={busy}
          onClick={() => void save('')}
          className="btn h-8 px-3 text-[11px]"
        >
          {t('settings.serverClear')}
        </button>
      </div>
      {note ? <div className="text-[11px] text-muted">{note}</div> : null}
      {status ? (
        <div className="rounded-lg border border-line bg-panel/50 p-3 text-[11px]">
          <div className="flex flex-wrap items-center gap-1.5">
            <span className={status.registered ? 'chip chip-on' : 'chip'}>
              {status.registered
                ? t('settings.serverRegistered')
                : t('settings.serverUnregistered')}
            </span>
            {status.registered ? (
              <span className={status.online ? 'chip chip-on' : 'chip'}>
                {status.online
                  ? t('settings.serverOnline')
                  : t('settings.serverOffline')}
              </span>
            ) : null}
          </div>
          {status.registered ? (
            <div className="mt-2 flex items-center gap-2">
              <span className="text-muted">{t('settings.serverId')}</span>
              <span className="font-mono text-[12px] text-ink">
                {status.display_id || status.device_id}
              </span>
              <button
                type="button"
                onClick={() => void copyId()}
                className="grid h-6 w-6 place-items-center rounded text-muted transition hover:bg-panel hover:text-ink"
                title={t('home.copy')}
                aria-label={t('home.copy')}
              >
                <Copy size={12} className={copied ? 'text-accent' : ''} />
              </button>
            </div>
          ) : null}
          {status.last_error ? (
            <div className="mt-2 break-words text-warn">
              {status.last_error}
            </div>
          ) : null}
        </div>
      ) : null}
      <div className="flex items-center justify-between gap-3 rounded-lg border border-line-soft px-3 py-2">
        <div className="min-w-0">
          <div className="text-[11px] text-ink">{t('settings.serverEnabled')}</div>
          <div className="text-[10px] leading-relaxed text-muted">
            {t('settings.serverEnabledHint')}
          </div>
        </div>
        <Switch
          checked={enabled}
          disabled={serverBusy}
          onChange={(value) => void toggleServer(value)}
          label={t('settings.serverEnabled')}
        />
      </div>

      <div className="rounded-lg border border-line bg-panel/50 p-3 text-[11px]">
        <div className="flex items-center justify-between gap-2">
          <span className="text-[11px] text-ink">{t('settings.domain')}</span>
          <div className="flex items-center gap-1">
            <button
              type="button"
              className="grid h-6 w-6 place-items-center rounded text-muted transition hover:bg-panel hover:text-ink disabled:opacity-40"
              onClick={() => void copyDomain()}
              title={t('settings.domainCopy')}
              disabled={!domainCode}
            >
              <Copy size={12} />
            </button>
            <button
              type="button"
              className="grid h-6 w-6 place-items-center rounded text-muted transition hover:bg-panel hover:text-ink disabled:opacity-40"
              onClick={() => void rotateDomain()}
              title={t('settings.domainRotate')}
              disabled={!enabled || !registered || domainBusy}
            >
              <RefreshCw size={12} />
            </button>
          </div>
        </div>
        <div className="mt-1 flex items-baseline gap-2">
          <span className="font-mono text-[14px] font-semibold tracking-[.12em] text-ink">
            {domainShown || '—'}
          </span>
          <span
            className={
              'text-[10px] ' +
              (!enabled
                ? 'text-muted'
                : !domainCode
                  ? 'text-muted'
                  : domainValid
                    ? 'text-accent'
                    : 'text-warn')
            }
          >
            {!enabled
              ? t('settings.serverDisabled')
              : !domainCode
                ? t('settings.domainPending')
                : domainValid
                  ? t('settings.domainActive')
                  : t('settings.domainRevoked')}
          </span>
        </div>
        <div className="mt-2 flex items-center gap-1.5">
          <input
            className="field h-7 min-w-0 flex-1 px-2 font-mono text-[11px] uppercase"
            value={domainInput}
            onChange={(event) => setDomainInput(event.target.value)}
            placeholder={t('settings.domainJoinPlaceholder')}
            maxLength={13}
            spellCheck={false}
            disabled={!enabled || !registered || domainBusy}
          />
          <button
            type="button"
            className="btn h-7 shrink-0 px-2 text-[10px]"
            onClick={() => void joinDomain()}
            disabled={
              !enabled || !registered || domainBusy || domainInput.trim() === ''
            }
          >
            {t('settings.domainJoin')}
          </button>
        </div>
        <div className="mt-1 text-[10px] leading-relaxed text-muted">
          {t('settings.domainHint')}
        </div>
      </div>

      <p className="text-[10px] leading-relaxed text-muted">
        {t('settings.serverTlsHint')}
      </p>
    </div>
  )
}

type SettingsSection =
  | 'general'
  | 'security'
  | 'network'
  | 'display'
  | 'service'
  | 'about'

const SETTINGS_SECTIONS: {
  id: SettingsSection
  label: string
  icon: typeof Settings2
}[] = [
  { id: 'general', label: 'settings.tabGeneral', icon: Settings2 },
  { id: 'security', label: 'settings.tabSecurity', icon: ShieldCheck },
  { id: 'network', label: 'settings.tabNetwork', icon: Network },
  { id: 'display', label: 'settings.tabDisplay', icon: Monitor },
  { id: 'service', label: 'settings.tabService', icon: Server },
  { id: 'about', label: 'settings.tabAbout', icon: Info },
]

export function SettingsPanel() {
  const { t } = useTranslation()
  const open = useAppStore((state) => state.settingsOpen)
  const setOpen = useAppStore((state) => state.setSettingsOpen)
  const localMode = useAppStore((state) => state.localMode)
  const security = useAppStore((state) => state.security)
  const refreshSecurity = useAppStore((state) => state.refreshSecurity)
  const saveSecurity = useAppStore((state) => state.saveSecurity)
  const setLongTermCode = useAppStore((state) => state.setLongTermCode)
  const setAuthCode = useAppStore((state) => state.setAuthCode)
  const device = useAppStore((state) => state.device)
  const setCodePolicy = useAppStore((state) => state.setCodePolicy)
  const [section, setSection] = useState<SettingsSection>('general')

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
      data-local-input="true"
      className="fixed inset-0 z-50 flex items-center justify-center bg-black/35 p-4 backdrop-blur-sm"
      onMouseDown={(event) => {
        if (event.target === event.currentTarget) setOpen(false)
      }}
    >
      <div className="flex max-h-[90vh] w-full max-w-3xl flex-col overflow-hidden rounded-2xl border border-line bg-surface shadow-2xl">
        <div className="flex shrink-0 items-center justify-between border-b border-line-soft px-5 py-3">
          <div className="flex items-center gap-2">
            <Settings2 size={16} className="text-accent" />
            <h2 className="text-base font-semibold">{t('settings.title')}</h2>
          </div>
          <button
            type="button"
            onClick={() => setOpen(false)}
            aria-label={t('settings.close')}
            className="grid h-8 w-8 place-items-center rounded-lg text-muted transition hover:bg-panel hover:text-ink"
          >
            <X size={16} />
          </button>
        </div>

        <div className="flex min-h-0 flex-1 flex-col md:flex-row">
          <nav className="flex shrink-0 gap-1 overflow-x-auto border-b border-line-soft bg-panel/30 p-2 md:w-44 md:flex-col md:border-b-0 md:border-r">
            {SETTINGS_SECTIONS.map(({ id, label, icon: Icon }) => (
              <button
                key={id}
                type="button"
                onClick={() => setSection(id)}
                aria-current={section === id ? 'page' : undefined}
                className={
                  'flex shrink-0 items-center gap-2 rounded-lg px-3 py-2 text-left text-[12px] transition ' +
                  (section === id
                    ? 'bg-accent-soft font-medium text-accent'
                    : 'text-muted hover:bg-panel hover:text-ink')
                }
              >
                <Icon size={15} />
                {t(label)}
              </button>
            ))}
          </nav>

          <div className="min-h-0 flex-1 overflow-y-auto p-5">
            {section === 'general' ? (
              <PreferencesControls section="general" />
            ) : null}

            {section === 'security' ? (
              localMode && security ? (
                <SecurityControls
                  key={`${security.allow_remote_access}:${security.allow_web_client}:${security.discovery_enabled}:${security.device_name}:${security.long_term_code}`}
                  settings={security}
                  onSave={saveSecurity}
                  onLongTerm={setLongTermCode}
                  onCustomCode={(uses, value) =>
                    setAuthCode('multi', uses, value)
                  }
                  code={device?.code ?? null}
                  onCodePolicy={setCodePolicy}
                />
              ) : (
                <p className="text-[12px] leading-relaxed text-muted">
                  {t('settings.localOnly')}
                </p>
              )
            ) : null}

            {section === 'network' ? (
              localMode ? (
                <div className="space-y-5">
                  <PreferencesControls section="network" />
                  <ServerControls />
                </div>
              ) : (
                <p className="text-[12px] leading-relaxed text-muted">
                  {t('settings.localOnly')}
                </p>
              )
            ) : null}

            {section === 'display' ? (
              <PreferencesControls section="display" />
            ) : null}

            {section === 'service' ? (
              localMode ? (
                <PreferencesControls section="service" />
              ) : (
                <p className="text-[12px] leading-relaxed text-muted">
                  {t('settings.localOnly')}
                </p>
              )
            ) : null}

            {section === 'about' ? <AboutPanel /> : null}

          </div>
        </div>
      </div>
    </div>
  )
}
