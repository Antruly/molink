import {
  ArrowUpRight,
  Check,
  Download,
  Code2,
  Globe,
  Loader2,
  RefreshCw,
  ShieldCheck,
  Star,
} from 'lucide-react'
import { useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { openExternal } from '../lib/shell'
import { isNewer } from '../lib/update'
import { useAppStore } from '../store/app'
import { Logo } from './Logo'

const REPOSITORY = 'https://github.com/Antruly/molink'
const WEBSITE = 'https://ssvr.top/molink'
const REPO_API = 'https://api.github.com/repos/Antruly/molink'
const RELEASE_API =
  'https://api.github.com/repos/Antruly/molink/releases/latest'

type CheckState = 'idle' | 'checking' | 'latest' | 'update' | 'error'

export function AboutPanel() {
  const { t } = useTranslation()
  const version = useAppStore(
    (state) => state.server?.version ?? state.device?.version ?? '—',
  )
  const [stars, setStars] = useState<number | null>(null)
  const [check, setCheck] = useState<CheckState>('idle')
  const [latest, setLatest] = useState<{ version: string; url: string } | null>(
    null,
  )

  // Stars are cosmetic: a blocked network or an API rate limit must leave the
  // page fully usable.
  useEffect(() => {
    let alive = true
    fetch(REPO_API, { headers: { Accept: 'application/vnd.github+json' } })
      .then((response) => (response.ok ? response.json() : null))
      .then((data) => {
        if (alive && data && typeof data.stargazers_count === 'number') {
          setStars(data.stargazers_count)
        }
      })
      .catch(() => {})
    return () => {
      alive = false
    }
  }, [])

  const checkUpdate = async () => {
    setCheck('checking')
    setLatest(null)
    try {
      const response = await fetch(RELEASE_API, {
        headers: { Accept: 'application/vnd.github+json' },
      })
      if (!response.ok) {
        setCheck('error')
        return
      }
      const release = (await response.json()) as {
        tag_name?: string
        html_url?: string
      }
      const tag = (release.tag_name ?? '').replace(/^v/i, '')
      if (!tag) {
        setCheck('error')
        return
      }
      if (isNewer(tag, version)) {
        setLatest({ version: tag, url: release.html_url ?? REPOSITORY })
        setCheck('update')
      } else {
        setCheck('latest')
      }
    } catch {
      setCheck('error')
    }
  }

  const row =
    'flex items-center justify-between gap-3 rounded-lg border border-line bg-panel/50 px-3 py-2.5'
  const link =
    'inline-flex items-center gap-1.5 text-[12px] font-medium text-accent transition hover:underline'

  return (
    <div className="space-y-5">
      <div className="flex items-center gap-3">
        <Logo size={40} />
        <div className="min-w-0">
          <div className="text-[16px] font-semibold text-ink">moLink</div>
          <div className="text-[11px] text-muted">
            {t('app.tagline')} · v{version}
          </div>
        </div>
        <button
          type="button"
          onClick={() => openExternal(REPOSITORY)}
          className="ml-auto inline-flex shrink-0 items-center gap-1.5 rounded-full border border-line bg-panel px-3 py-1.5 text-[11px] font-medium text-ink transition hover:border-accent/40 hover:text-accent"
        >
          <Star size={13} />
          {stars === null ? t('settings.aboutStarsUnknown') : stars}
        </button>
      </div>

      <div className="space-y-2">
        <div className={row}>
          <span className="flex items-center gap-2 text-[12px] text-muted">
            <Code2 size={14} />
            {t('settings.aboutRepository')}
          </span>
          <button
            type="button"
            onClick={() => openExternal(REPOSITORY)}
            className={link}
          >
            {t('settings.aboutOpen')}
            <ArrowUpRight size={12} />
          </button>
        </div>
        <div className={row}>
          <span className="flex items-center gap-2 text-[12px] text-muted">
            <Globe size={14} />
            {t('settings.aboutWebsite')}
          </span>
          <button
            type="button"
            onClick={() => openExternal(WEBSITE)}
            className={link}
          >
            ssvr.top/molink
            <ArrowUpRight size={12} />
          </button>
        </div>
        <div className={row}>
          <span className="flex items-center gap-2 text-[12px] text-muted">
            <ShieldCheck size={14} />
            {t('settings.aboutLicense')}
          </span>
          <span className="font-mono text-[11px] text-ink">MIT</span>
        </div>
      </div>

      <div className="rounded-xl border border-accent/30 bg-accent-soft/40 p-3.5">
        <div className="flex items-start gap-2">
          <Star size={15} className="mt-0.5 shrink-0 text-accent" />
          <div className="min-w-0 flex-1">
            <div className="text-[12px] font-medium text-ink">
              {t('settings.aboutStarHint')}
            </div>
            <div className="mt-1 flex flex-wrap items-center gap-2">
              <button
                type="button"
                onClick={() => openExternal(REPOSITORY)}
                className="btn btn-primary h-8 gap-1.5 px-3 text-[11px]"
              >
                <Star size={13} />
                {t('settings.aboutStar')}
              </button>
              {stars !== null ? (
                <span className="text-[11px] text-muted">
                  {t('settings.aboutStarsCount', { count: stars })}
                </span>
              ) : null}
            </div>
          </div>
        </div>
      </div>

      <div className="flex flex-wrap items-center gap-2 rounded-lg border border-line bg-panel/50 px-3 py-2.5">
        <div className="min-w-0 flex-1">
          <div className="text-[12px] font-medium text-ink">
            {t('settings.aboutCheckUpdate')}
          </div>
          <div className="mt-0.5 text-[11px] text-muted">
            {check === 'checking'
              ? t('settings.aboutChecking')
              : check === 'latest'
                ? t('settings.aboutLatest')
                : check === 'update' && latest
                  ? t('settings.aboutUpdate', { version: latest.version })
                  : check === 'error'
                    ? t('settings.aboutUpdateFailed')
                    : t('settings.aboutCurrent', { version })}
          </div>
        </div>
        {check === 'update' && latest ? (
          <button
            type="button"
            onClick={() => openExternal(latest.url)}
            className="btn btn-primary h-8 gap-1.5 px-2.5 text-[11px]"
          >
            <Download size={13} />
            {t('settings.aboutUpdateOpen')}
          </button>
        ) : (
          <button
            type="button"
            onClick={() => void checkUpdate()}
            disabled={check === 'checking'}
            className="btn h-8 gap-1.5 px-2.5 text-[11px]"
          >
            {check === 'checking' ? (
              <Loader2 size={13} className="animate-spin" />
            ) : check === 'latest' ? (
              <Check size={13} className="text-emerald-500" />
            ) : (
              <RefreshCw size={13} />
            )}
            {check === 'checking'
              ? t('settings.aboutChecking')
              : t('settings.aboutCheckUpdate')}
          </button>
        )}
      </div>

      <div className="rounded-lg border border-line bg-panel/40 p-3.5">
        <div className="flex items-center gap-2 text-[12px] font-medium text-ink">
          <ShieldCheck size={14} className="text-accent" />
          {t('settings.aboutPrivacyTitle')}
        </div>
        <p className="mt-1.5 text-[11px] leading-relaxed text-muted">
          {t('settings.aboutPrivacy')}
        </p>
      </div>
    </div>
  )
}
