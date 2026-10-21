import {
  Eye,
  EyeOff,
  Loader2,
  LockKeyhole,
  Moon,
  RefreshCw,
  Sun,
} from 'lucide-react'
import { useState, type FormEvent } from 'react'
import { useTranslation } from 'react-i18next'
import { useAppStore } from '../store/app'
import { Logo } from './Logo'

export function AuthGate() {
  const { t } = useTranslation()
  const phase = useAppStore((state) => state.authPhase)
  const server = useAppStore((state) => state.server)
  const stateError = useAppStore((state) => state.stateError)
  const authenticate = useAppStore((state) => state.authenticate)
  const boot = useAppStore((state) => state.boot)
  const theme = useAppStore((state) => state.theme)
  const toggleTheme = useAppStore((state) => state.toggleTheme)

  const [code, setCode] = useState('')
  const [show, setShow] = useState(false)
  const [busy, setBusy] = useState(false)
  const locked = phase === 'locked'
  const unreachable = !server

  const submit = async (event: FormEvent) => {
    event.preventDefault()
    if (code.trim() === '') return
    setBusy(true)
    const ok = await authenticate(code.trim())
    setBusy(false)
    if (!ok) setCode('')
  }

  const errorText = stateError
    ? String(
        t(`auth.${stateError}`, {
          defaultValue: t('auth.invalid_token'),
        }),
      )
    : null

  return (
    <div className="relative flex min-h-0 flex-1 items-center justify-center px-4 py-10">
      <div className="w-full max-w-md rounded-2xl border border-line bg-surface p-6 shadow-2xl sm:p-8">
        <div className="mb-6 flex items-start justify-between">
          <div className="flex items-center gap-3">
            <Logo size={38} />
            <div>
              <div className="text-[15px] font-semibold tracking-tight">
                moLink
              </div>
              <div className="text-[11px] uppercase tracking-[0.18em] text-muted">
                {t('app.tagline')}
              </div>
            </div>
          </div>
          <button
            type="button"
            onClick={toggleTheme}
            className="btn btn-quiet h-9 w-9 !px-0"
            aria-label={t('settings.theme')}
            title={t('settings.theme')}
          >
            {theme === 'dark' ? <Sun size={16} /> : <Moon size={16} />}
          </button>
        </div>

        <div className="mb-5 flex items-center gap-2 text-accent">
          <LockKeyhole size={18} />
          <h1 className="text-lg font-semibold text-ink">
            {locked ? t('auth.lockedTitle') : t('auth.loginTitle')}
          </h1>
        </div>
        <p className="text-[13px] leading-relaxed text-muted">
          {locked ? t('auth.lockedSubtitle') : t('auth.loginSubtitle')}
        </p>

        {server ? (
          <div className="mt-4 flex items-center gap-2 rounded-lg bg-panel px-3 py-2 font-mono text-[12px] text-muted">
            <span className="text-[10px] uppercase tracking-[0.16em]">
              {t('auth.device')}
            </span>
            <span className="text-ink">{server.device_id}</span>
          </div>
        ) : null}

        <form onSubmit={submit} className="mt-5 space-y-4">
          <label className="block">
            <span className="mb-1.5 block text-[10px] uppercase tracking-[0.16em] text-muted">
              {t('auth.code')}
            </span>
            <div className="relative">
              <input
                value={code}
                onChange={(event) => setCode(event.target.value)}
                type={show ? 'text' : 'password'}
                autoFocus
                autoComplete="off"
                spellCheck={false}
                className="field pr-10 font-mono tracking-[0.2em]"
              />
              <button
                type="button"
                onClick={() => setShow((value) => !value)}
                className="absolute inset-y-0 right-0 grid w-10 place-items-center text-muted transition hover:text-ink"
                aria-label={show ? t('home.hide') : t('home.show')}
              >
                {show ? <EyeOff size={14} /> : <Eye size={14} />}
              </button>
            </div>
          </label>

          {errorText ? (
            <div className="rounded-lg border border-danger/35 bg-danger/10 px-3 py-2 text-[12px] text-danger">
              {errorText}
            </div>
          ) : null}

          <button
            type="submit"
            disabled={busy || code.trim() === ''}
            className="btn btn-primary w-full"
          >
            {busy ? <Loader2 size={15} className="animate-spin" /> : null}
            {locked ? t('auth.unlock') : t('auth.verify')}
          </button>
        </form>

        {unreachable ? (
          <div className="mt-4 flex items-center justify-between gap-3 rounded-lg bg-panel px-3 py-2 text-[11px] text-muted">
            <span>{t('auth.core_unreachable')}</span>
            <button type="button" onClick={() => void boot()} className="btn">
              <RefreshCw size={13} />
              {t('auth.retry')}
            </button>
          </div>
        ) : locked ? null : (
          <p className="mt-4 text-[11px] leading-relaxed text-muted/80">
            {t('auth.remoteNote')}
          </p>
        )}
      </div>
    </div>
  )
}
