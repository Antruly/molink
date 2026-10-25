import { Monitor, FolderUp, MessageSquare, Send, KeyRound } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import type { SessionIntent } from '../../store/app'

const MODES: Array<{ value: SessionIntent; icon: typeof Monitor; label: string }> = [
  { value: 'desktop', icon: Monitor, label: 'simple.modeDesktop' },
  { value: 'files', icon: FolderUp, label: 'simple.modeFiles' },
  { value: 'chat', icon: MessageSquare, label: 'simple.modeChat' },
]

export function ConnectCard({
  mode,
  setMode,
  input,
  setInput,
  code,
  setCode,
  busy,
  error,
  onSubmit,
}: {
  mode: SessionIntent
  setMode: (mode: SessionIntent) => void
  input: string
  setInput: (value: string) => void
  code: string
  setCode: (value: string) => void
  busy: boolean
  error: string | null
  onSubmit: () => void
}) {
  const { t } = useTranslation()
  const requestAccess = code.trim() === ''
  return (
    <section className="card h-full p-3">
      <div className="flex flex-wrap items-center justify-between gap-2">
        <div>
          <h2 className="text-[15px] font-semibold text-ink">{t('simple.connectTitle')}</h2>
          <p className="mt-0.5 text-[10px] text-muted">{t('simple.connectHint')}</p>
        </div>
        <div className="flex rounded-full border border-line bg-panel/70 p-0.5">
          {MODES.map((item) => {
            const Icon = item.icon
            return (
              <button
                key={item.value}
                type="button"
                onClick={() => setMode(item.value)}
                className={
                  'flex items-center gap-1 rounded-full px-2.5 py-1 text-[11px] transition ' +
                  (mode === item.value
                    ? 'bg-accent-soft text-accent'
                    : 'text-muted hover:text-ink')
                }
              >
                <Icon size={12} />
                {t(item.label)}
              </button>
            )
          })}
        </div>
      </div>

      <form
        className="mt-4 space-y-3"
        onSubmit={(event) => {
          event.preventDefault()
          if (!busy) onSubmit()
        }}
      >
        <div className="relative">
          <input
            className="field h-12 pl-3 pr-10 font-mono text-[14px]"
            value={input}
            onChange={(event) => setInput(event.target.value)}
            placeholder={t('simple.targetPlaceholder')}
            autoFocus
            spellCheck={false}
          />
          <button
            type="submit"
            className="absolute right-1.5 top-1.5 grid h-9 w-9 place-items-center rounded-lg bg-accent text-on-accent transition hover:opacity-90 disabled:opacity-50"
            disabled={busy || input.trim() === ''}
            title={requestAccess ? t('simple.requestAccess') : t('simple.connect')}
          >
            <Send size={16} />
          </button>
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <label className="relative flex min-w-0 flex-1 items-center gap-1.5 rounded-xl border border-line bg-panel/50 px-2.5 py-1.5">
            <KeyRound size={13} className="shrink-0 text-muted" />
            <input
              className="min-w-0 flex-1 bg-transparent text-[12px] text-ink outline-none placeholder:text-muted/60"
              value={code}
              onChange={(event) => setCode(event.target.value)}
              placeholder={t('simple.codePlaceholder')}
              autoComplete="off"
            />
          </label>
          <span className="text-[10px] text-muted">
            {requestAccess ? t('simple.requestHint') : t('simple.codeHint')}
          </span>
        </div>
        {error ? (
          <div className="rounded-lg border border-danger/40 bg-danger/10 px-3 py-2 text-[11px] text-danger">
            {error}
          </div>
        ) : null}
        <button
          type="button"
          className="btn btn-primary h-9 w-full gap-1.5 text-[12px]"
          onClick={onSubmit}
          disabled={busy || input.trim() === ''}
        >
          <Send size={13} />
          {busy
            ? t('simple.connecting')
            : requestAccess
              ? t('simple.requestAccess')
              : t('simple.connect')}
        </button>
        <div className="text-center text-[10px] text-muted">
          {t('simple.portHint')}
        </div>
      </form>
    </section>
  )
}
