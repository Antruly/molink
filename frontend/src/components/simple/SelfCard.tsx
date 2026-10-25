import { useRef, useState } from 'react'
import {
  Check,
  Copy,
  Eye,
  EyeOff,
  Pencil,
  RefreshCw,
  Server,
  ShieldCheck,
} from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { useAppStore } from '../../store/app'
import { Avatar, AvatarPicker } from './Avatar'

export function SelfCard() {
  const { t } = useTranslation()
  const device = useAppStore((state) => state.device)
  const security = useAppStore((state) => state.security)
  const server = useAppStore((state) => state.server)
  const localMode = useAppStore((state) => state.localMode)
  const token = useAppStore((state) => state.token)
  const saveSecurity = useAppStore((state) => state.saveSecurity)
  const setAuthCode = useAppStore((state) => state.setAuthCode)
  const refreshDevice = useAppStore((state) => state.refreshDevice)
  const setAvatarValue = useAppStore((state) => state.setAvatarValue)
  const setSettingsOpen = useAppStore((state) => state.setSettingsOpen)

  // The name follows the core until the user starts editing; a draft keeps
  // keystrokes local without a state-syncing effect.
  const [nameDraft, setNameDraft] = useState<string | null>(null)
  const [editing, setEditing] = useState(false)
  const [codeVisible, setCodeVisible] = useState(false)
  const [copied, setCopied] = useState<'id' | 'code' | null>(null)
  const [notice, setNotice] = useState<string | null>(null)
  const [pickerOpen, setPickerOpen] = useState(false)
  const inputRef = useRef<HTMLInputElement>(null)

  const storedName = security?.device_name ?? device?.device_name ?? ''
  const name = nameDraft ?? storedName

  if (!localMode) return null

  const serverInfo = server?.server
  const enabled = serverInfo?.enabled ?? true
  const registered = serverInfo?.registered ?? false
  const online = serverInfo?.online ?? false
  const directoryId = serverInfo?.display_id || ''
  const localId = device?.device_id ?? ''
  const shownId = registered && directoryId ? directoryId : localId
  const ipAddress = device?.addresses?.[0]
    ? `${device.addresses[0]}:${device.port}`
    : ''
  const code = device?.code
  const avatar = device?.avatar

  const flash = (message: string) => {
    setNotice(message)
    window.setTimeout(() => setNotice(null), 2400)
  }

  const copy = async (value: string, kind: 'id' | 'code') => {
    if (!value) return
    try {
      await navigator.clipboard.writeText(value.replace(/-/g, ''))
      setCopied(kind)
      window.setTimeout(() => setCopied(null), 1500)
    } catch {
      flash(t('simple.copyFailed'))
    }
  }

  const saveName = async () => {
    setEditing(false)
    const trimmed = name.trim()
    if (!security || !trimmed || trimmed === security.device_name) {
      setNameDraft(null)
      return
    }
    const saved = await saveSecurity({
      allowRemote: security.allow_remote_access,
      allowWeb: security.allow_web_client,
      discovery: security.discovery_enabled,
      allowAccessRequests: security.allow_access_requests,
      name: trimmed,
    })
    setNameDraft(null)
    if (saved) {
      flash(t('simple.nameSaved'))
      void refreshDevice()
    }
  }

  const serverLabel = !enabled
    ? t('simple.lanOnly')
    : registered && online
      ? t('simple.serverOnline')
      : registered
        ? t('simple.serverConnecting')
        : t('simple.serverOffline')

  return (
    <section className="card h-full p-3">
      <div className="flex items-start gap-3">
        <button
          type="button"
          className="relative rounded-2xl transition hover:scale-[1.03]"
          onClick={() => setPickerOpen(true)}
          title={t('simple.chooseAvatar')}
        >
          <Avatar value={avatar} name={name || localId} platform={device?.platform} size={58} />
          <span className="absolute -bottom-1 -right-1 grid h-5 w-5 place-items-center rounded-full border border-line bg-surface text-muted">
            <Pencil size={10} />
          </span>
        </button>
        <div className="min-w-0 flex-1">
          {editing ? (
            <input
              ref={inputRef}
              className="field h-8 px-2 text-[15px] font-semibold"
              value={name}
              autoFocus
              maxLength={64}
              onChange={(event) => setNameDraft(event.target.value)}
              onBlur={() => void saveName()}
              onKeyDown={(event) => {
                if (event.key === 'Enter') void saveName()
                if (event.key === 'Escape') {
                  setNameDraft(null)
                  setEditing(false)
                }
              }}
            />
          ) : (
            <button
              type="button"
              className="flex max-w-full items-center gap-1.5 text-left"
              onClick={() => {
                setEditing(true)
                window.setTimeout(() => inputRef.current?.select(), 0)
              }}
            >
              <span className="truncate text-[16px] font-semibold text-ink">
                {name || t('simple.unnamed')}
              </span>
              <Pencil size={11} className="shrink-0 text-muted" />
            </button>
          )}
          <div className="mt-1 flex flex-wrap items-center gap-1.5 text-[10px] text-muted">
            <span className="chip !px-1.5 !py-0 font-mono">{device?.platform || 'device'}</span>
            {enabled ? (
              <span className="chip !px-1.5 !py-0 text-accent">{serverLabel}</span>
            ) : (
              <span className="chip !px-1.5 !py-0 text-warn">{serverLabel}</span>
            )}
          </div>
          <div className="mt-2 flex items-center gap-1.5">
            <span className="min-w-0 truncate font-mono text-[20px] font-semibold tracking-wide text-ink">
              {shownId || t('simple.idPending')}
            </span>
            {shownId ? (
              <button
                type="button"
                className="grid h-6 w-6 shrink-0 place-items-center rounded text-muted hover:bg-panel hover:text-ink"
                onClick={() => void copy(shownId, 'id')}
                title={t('simple.copyId')}
              >
                {copied === 'id' ? <Check size={12} className="text-accent" /> : <Copy size={12} />}
              </button>
            ) : null}
          </div>
          {ipAddress ? (
            <div className="mt-0.5 font-mono text-[10px] text-muted">{ipAddress}</div>
          ) : null}
        </div>
      </div>

      <div className="mt-3 rounded-xl border border-line bg-panel/55 p-2.5">
        <div className="flex items-center justify-between gap-2">
          <span className="flex items-center gap-1.5 text-[11px] text-muted">
            <ShieldCheck size={13} className="text-accent" />
            {t('simple.code')}
          </span>
          <div className="flex items-center gap-1">
            <button
              type="button"
              className="btn btn-quiet h-6 w-6 !px-0"
              onClick={() => setCodeVisible((value) => !value)}
              title={codeVisible ? t('simple.hideCode') : t('simple.showCode')}
            >
              {codeVisible ? <EyeOff size={12} /> : <Eye size={12} />}
            </button>
            <button
              type="button"
              className="btn btn-quiet h-6 w-6 !px-0"
              onClick={() => void copy(code?.value ?? '', 'code')}
              title={t('simple.copyCode')}
              disabled={!code?.value}
            >
              {copied === 'code' ? <Check size={12} className="text-accent" /> : <Copy size={12} />}
            </button>
            <button
              type="button"
              className="btn btn-quiet h-6 w-6 !px-0"
              onClick={() => void setAuthCode(code?.mode ?? 'multi', code?.uses ?? 20)}
              title={t('simple.refreshCode')}
            >
              <RefreshCw size={12} />
            </button>
          </div>
        </div>
        <div className="mt-2 flex items-center gap-2">
          <span className="min-w-0 flex-1 truncate font-mono text-[22px] font-semibold tracking-[.18em] text-ink">
            {codeVisible ? code?.value || '—' : '••••••'}
          </span>
          <div className="flex gap-1">
            {(['single', 'multi'] as const).map((mode) => (
              <button
                key={mode}
                type="button"
                className={
                  'rounded-full px-2 py-1 text-[10px] transition ' +
                  (code?.mode === mode
                    ? 'bg-accent-soft text-accent'
                    : 'text-muted hover:bg-panel')
                }
                onClick={() => void setAuthCode(mode, mode === 'single' ? 1 : 20)}
              >
                {mode === 'single' ? t('simple.single') : t('simple.multi')}
              </button>
            ))}
          </div>
        </div>
        <div className="mt-1 text-[10px] text-muted">
          {code ? t('simple.codeUses', { remaining: code.remaining, uses: code.uses }) : ''}
        </div>
      </div>

      <button
        type="button"
        className="mt-3 flex w-full items-center justify-center gap-1.5 text-[10px] text-muted transition hover:text-ink"
        onClick={() => setSettingsOpen(true)}
      >
        <Server size={11} />
        {t('simple.moreSettings')}
      </button>

      {notice ? (
        <div className="mt-2 text-center text-[10px] text-accent">{notice}</div>
      ) : null}

      <AvatarPicker
        open={pickerOpen}
        value={avatar}
        name={name || localId}
        onClose={() => setPickerOpen(false)}
        onSelect={async (value) => {
          if (!token) return
          const ok = await setAvatarValue(value)
          if (ok) {
            flash(t('simple.avatarSaved'))
            void refreshDevice()
          }
        }}
      />
    </section>
  )
}
