import { Eye, EyeOff, Loader2, ShieldCheck, Terminal } from 'lucide-react'
import { useState, type FormEvent } from 'react'
import { useTranslation } from 'react-i18next'
import { useSessionStore } from '../store/session'

export function ConnectPanel() {
  const { t } = useTranslation()
  const phase = useSessionStore((state) => state.phase)
  const host = useSessionStore((state) => state.host)
  const port = useSessionStore((state) => state.port)
  const token = useSessionStore((state) => state.token)
  const error = useSessionStore((state) => state.error)
  const setField = useSessionStore((state) => state.setField)
  const connect = useSessionStore((state) => state.connect)

  const [showToken, setShowToken] = useState(false)
  const connecting = phase === 'connecting'

  const submit = (event: FormEvent) => {
    event.preventDefault()
    connect()
  }

  const errorText = error
    ? String(t(`error.${error}`, { defaultValue: error }))
    : null

  return (
    <div className="flex flex-1 items-center justify-center px-5 py-10">
      <div className="w-full max-w-lg">
        <div className="mb-6 flex items-center gap-2 text-[11px] uppercase tracking-[0.18em] text-fog">
          <ShieldCheck size={14} className="text-signal" />
          {t('connect.loopback')}
        </div>

        <form
          onSubmit={submit}
          className="rounded-2xl border border-line bg-panel/80 p-6 shadow-[0_24px_80px_-32px_rgba(0,0,0,0.9)] backdrop-blur sm:p-8"
        >
          <h1 className="text-xl font-semibold tracking-tight">
            {t('connect.title')}
          </h1>
          <p className="mt-2 text-sm leading-relaxed text-fog">
            {t('connect.subtitle')}
          </p>

          <div className="mt-7 grid grid-cols-1 gap-4 sm:grid-cols-[1fr_120px]">
            <label className="block">
              <span className="mb-1.5 block text-[11px] uppercase tracking-[0.16em] text-fog">
                {t('connect.host')}
              </span>
              <input
                value={host}
                onChange={(event) => setField('host', event.target.value)}
                autoComplete="off"
                spellCheck={false}
                className="w-full rounded-md border border-line bg-ink/70 px-3 py-2 font-mono text-sm text-mist outline-none transition focus:border-signal/60 focus:ring-2 focus:ring-signal/15"
              />
            </label>

            <label className="block">
              <span className="mb-1.5 block text-[11px] uppercase tracking-[0.16em] text-fog">
                {t('connect.port')}
              </span>
              <input
                value={port}
                onChange={(event) => setField('port', event.target.value)}
                inputMode="numeric"
                autoComplete="off"
                className="w-full rounded-md border border-line bg-ink/70 px-3 py-2 font-mono text-sm text-mist outline-none transition focus:border-signal/60 focus:ring-2 focus:ring-signal/15"
              />
            </label>
          </div>

          <label className="mt-4 block">
            <span className="mb-1.5 block text-[11px] uppercase tracking-[0.16em] text-fog">
              {t('connect.token')}
            </span>
            <div className="relative">
              <input
                value={token}
                onChange={(event) => setField('token', event.target.value)}
                type={showToken ? 'text' : 'password'}
                autoComplete="off"
                spellCheck={false}
                className="w-full rounded-md border border-line bg-ink/70 px-3 py-2 pr-10 font-mono text-sm text-mist outline-none transition focus:border-signal/60 focus:ring-2 focus:ring-signal/15"
              />
              <button
                type="button"
                onClick={() => setShowToken((value) => !value)}
                aria-label={
                  showToken ? t('connect.hideToken') : t('connect.showToken')
                }
                className="absolute inset-y-0 right-0 grid w-10 place-items-center text-fog transition hover:text-mist"
              >
                {showToken ? <EyeOff size={15} /> : <Eye size={15} />}
              </button>
            </div>
            <span className="mt-1.5 block text-[11px] leading-relaxed text-fog/70">
              {t('connect.tokenHint')}
            </span>
          </label>

          {errorText ? (
            <div className="mt-5 rounded-md border border-alert/40 bg-alert/10 px-3 py-2 text-sm text-alert">
              {errorText}
            </div>
          ) : null}

          <button
            type="submit"
            disabled={connecting}
            className="mt-6 flex w-full items-center justify-center gap-2 rounded-md bg-signal px-4 py-2.5 text-sm font-semibold text-ink transition hover:bg-signal/90 disabled:cursor-not-allowed disabled:opacity-60"
          >
            {connecting ? <Loader2 size={16} className="animate-spin" /> : null}
            {connecting ? t('connect.connecting') : t('connect.submit')}
          </button>

          <div className="mt-5 flex items-start gap-2 rounded-md border border-line-soft bg-ink/60 p-3 font-mono text-[11px] leading-relaxed text-fog">
            <Terminal size={14} className="mt-0.5 shrink-0 text-signal/80" />
            <span>{t('connect.hint')}</span>
          </div>
        </form>
      </div>
    </div>
  )
}
