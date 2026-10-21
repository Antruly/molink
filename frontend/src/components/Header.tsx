import { Languages, Monitor } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { setLanguage } from '../i18n'

export function Header() {
  const { t, i18n } = useTranslation()
  const isChinese = i18n.language.toLowerCase().startsWith('zh')

  return (
    <header className="flex items-center justify-between border-b border-line-soft px-5 py-4 sm:px-8">
      <div className="flex items-center gap-3">
        <div className="grid h-9 w-9 place-items-center rounded-lg border border-line bg-panel-2 text-signal">
          <Monitor size={18} strokeWidth={1.75} />
        </div>
        <div>
          <div className="text-sm font-semibold uppercase tracking-[0.22em]">
            molink
          </div>
          <div className="text-[11px] text-fog">{t('app.tagline')}</div>
        </div>
      </div>

      <div className="flex items-center gap-2">
        <span className="hidden font-mono text-[11px] text-fog/80 sm:inline">
          {t('app.version')}
        </span>
        <button
          type="button"
          onClick={() => setLanguage(isChinese ? 'en' : 'zh-CN')}
          className="flex items-center gap-1.5 rounded-md border border-line bg-panel px-2.5 py-1.5 text-[11px] text-fog transition hover:border-signal/50 hover:text-mist"
        >
          <Languages size={13} />
          {isChinese ? 'EN' : '中文'}
        </button>
      </div>
    </header>
  )
}
