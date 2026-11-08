import {
  ArrowLeft,
  Check,
  Copy,
  Download,
  Search,
  Trash2,
} from 'lucide-react'
import { useEffect, useMemo, useState, useSyncExternalStore } from 'react'
import { useTranslation } from 'react-i18next'
import {
  buildDiagText,
  clearDiag,
  formatDiagLine,
  getDiagEntries,
  MAX_DIAG_ENTRIES,
  subscribeDiag,
  type DiagEntry,
} from '../lib/diag-log'
import { useAppStore } from '../store/app'

type Filter = 'all' | 'warn' | 'error'

function useDiagEntries(): readonly DiagEntry[] {
  return useSyncExternalStore(subscribeDiag, getDiagEntries, getDiagEntries)
}

async function copyText(text: string): Promise<boolean> {
  try {
    if (navigator.clipboard?.writeText) {
      await navigator.clipboard.writeText(text)
      return true
    }
  } catch {
    // Fall through to the selection-based copy below.
  }
  try {
    const area = document.createElement('textarea')
    area.value = text
    area.setAttribute('readonly', '')
    area.style.position = 'fixed'
    area.style.top = '-1000px'
    area.style.opacity = '0'
    document.body.appendChild(area)
    area.select()
    const ok = document.execCommand('copy')
    area.remove()
    return ok
  } catch {
    return false
  }
}

function downloadText(name: string, text: string): void {
  const blob = new Blob([text], { type: 'text/plain;charset=utf-8' })
  const url = URL.createObjectURL(blob)
  const link = document.createElement('a')
  link.href = url
  link.download = name
  link.style.display = 'none'
  document.body.appendChild(link)
  link.click()
  link.remove()
  window.setTimeout(() => URL.revokeObjectURL(url), 10000)
}

function exportFileName(version: string): string {
  const stamp = new Date()
  const pad = (value: number) => String(value).padStart(2, '0')
  const date =
    `${stamp.getFullYear()}${pad(stamp.getMonth() + 1)}${pad(stamp.getDate())}` +
    `-${pad(stamp.getHours())}${pad(stamp.getMinutes())}${pad(stamp.getSeconds())}`
  const safeVersion = version.replace(/[^\w.-]+/g, '')
  return `molink-log-${safeVersion ? `${safeVersion}-` : ''}${date}.txt`
}

/// The log viewer behind the low-contrast "运行日志" entry at the bottom of
/// the settings dialog. It renders the local diagnostics ring buffer and can
/// copy or export what is on screen; nothing leaves the device by itself.
export function DiagLogPanel({ onBack }: { onBack?: () => void }) {
  const { t } = useTranslation()
  const entries = useDiagEntries()
  const version = useAppStore(
    (state) => state.server?.version ?? state.device?.version ?? '',
  )
  const [filter, setFilter] = useState<Filter>('all')
  const [query, setQuery] = useState('')
  const [copied, setCopied] = useState(false)
  const [clearArmed, setClearArmed] = useState(false)

  useEffect(() => {
    if (!clearArmed) return
    const timer = window.setTimeout(() => setClearArmed(false), 4000)
    return () => window.clearTimeout(timer)
  }, [clearArmed])

  useEffect(() => {
    if (!copied) return
    const timer = window.setTimeout(() => setCopied(false), 1600)
    return () => window.clearTimeout(timer)
  }, [copied])

  const visible = useMemo(() => {
    const needle = query.trim().toLowerCase()
    const filtered = entries.filter((entry) => {
      if (filter === 'error' && entry.lvl !== 'error') return false
      if (filter === 'warn' && entry.lvl === 'info') return false
      if (needle === '') return true
      return formatDiagLine(entry).toLowerCase().includes(needle)
    })
    return filtered.reverse()
  }, [entries, filter, query])

  const counts = useMemo(() => {
    let error = 0
    let warn = 0
    for (const entry of entries) {
      if (entry.lvl === 'error') error += 1
      else if (entry.lvl === 'warn') warn += 1
    }
    return { error, warn }
  }, [entries])

  const exportText = () => {
    const noteParts: string[] = []
    if (filter !== 'all') noteParts.push(`filter=${filter}`)
    if (query.trim() !== '') noteParts.push(`query="${query.trim()}"`)
    return buildDiagText({
      version,
      entries: [...visible].reverse(),
      note: noteParts.length > 0 ? noteParts.join(' ') : undefined,
    })
  }

  const onCopy = async () => {
    if (await copyText(exportText())) setCopied(true)
  }

  const onExport = () => {
    downloadText(exportFileName(version), exportText())
  }

  const onClear = () => {
    if (!clearArmed) {
      setClearArmed(true)
      return
    }
    setClearArmed(false)
    clearDiag()
  }

  const filterButton = (id: Filter, label: string, count: number) => (
    <button
      type="button"
      onClick={() => setFilter(id)}
      className={filter === id ? 'chip chip-on' : 'chip'}
      aria-pressed={filter === id}
    >
      {label}
      <span className="font-mono text-[10px] opacity-70">{count}</span>
    </button>
  )

  return (
    <div className="space-y-3">
      <div className="flex items-start gap-3">
        {onBack ? (
          <button
            type="button"
            onClick={onBack}
            aria-label={t('diagnostics.back')}
            className="mt-0.5 grid h-7 w-7 shrink-0 place-items-center rounded-lg text-muted transition hover:bg-panel hover:text-ink"
          >
            <ArrowLeft size={15} />
          </button>
        ) : null}
        <div className="min-w-0 flex-1">
          <div className="text-[13px] font-semibold text-ink">
            {t('diagnostics.title')}
          </div>
          <div className="mt-0.5 text-[10px] leading-relaxed text-muted">
            {t('diagnostics.hint', { max: MAX_DIAG_ENTRIES })}
          </div>
        </div>
        <span className="chip shrink-0 font-mono">
          {entries.length}/{MAX_DIAG_ENTRIES}
        </span>
      </div>

      <div className="flex flex-wrap items-center gap-1.5">
        {filterButton('all', t('diagnostics.filterAll'), entries.length)}
        {filterButton('warn', t('diagnostics.filterWarn'), counts.warn + counts.error)}
        {filterButton('error', t('diagnostics.filterError'), counts.error)}
        <div className="relative min-w-32 flex-1">
          <Search
            size={12}
            className="pointer-events-none absolute left-2 top-1/2 -translate-y-1/2 text-muted"
          />
          <input
            className="field h-7 w-full pl-7 text-[11px]"
            value={query}
            onChange={(event) => setQuery(event.target.value)}
            placeholder={t('diagnostics.search')}
            spellCheck={false}
          />
        </div>
        <button
          type="button"
          onClick={() => void onCopy()}
          disabled={visible.length === 0}
          className="btn h-7 gap-1 px-2 text-[11px]"
        >
          {copied ? <Check size={12} className="text-accent" /> : <Copy size={12} />}
          {copied ? t('diagnostics.copied') : t('diagnostics.copy')}
        </button>
        <button
          type="button"
          onClick={onExport}
          disabled={visible.length === 0}
          className="btn h-7 gap-1 px-2 text-[11px]"
        >
          <Download size={12} />
          {t('diagnostics.export')}
        </button>
        <button
          type="button"
          onClick={onClear}
          disabled={entries.length === 0}
          className={
            'btn h-7 gap-1 px-2 text-[11px]' +
            (clearArmed ? ' border-danger/60 text-danger' : '')
          }
        >
          <Trash2 size={12} />
          {clearArmed ? t('diagnostics.clearConfirm') : t('diagnostics.clear')}
        </button>
      </div>

      <div className="h-[46vh] min-h-52 overflow-y-auto rounded-lg border border-line bg-panel/40 p-2">
        {visible.length === 0 ? (
          <div className="grid h-full min-h-40 place-items-center px-4 text-center text-[11px] text-muted">
            {entries.length === 0 ? t('diagnostics.empty') : t('diagnostics.noMatch')}
          </div>
        ) : (
          <div className="space-y-0.5 font-mono text-[10px] leading-relaxed">
            {visible.map((entry, index) => (
              <div
                key={`${entry.t}-${index}`}
                className={
                  'whitespace-pre-wrap break-words rounded px-1 py-0.5 ' +
                  (entry.lvl === 'error'
                    ? 'text-danger'
                    : entry.lvl === 'warn'
                      ? 'text-warn'
                      : 'text-muted')
                }
              >
                {formatDiagLine(entry)}
              </div>
            ))}
          </div>
        )}
      </div>

      <p className="text-[10px] leading-relaxed text-muted">
        {t('diagnostics.privacy')}
      </p>
    </div>
  )
}
