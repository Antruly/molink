import { Home, Moon, Settings2, Sun } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { useAppStore } from '../store/app'
import { Logo } from './Logo'

export function AppHeader({ showHome = true }: { showHome?: boolean }) {
  const { t } = useTranslation()
  const theme = useAppStore((state) => state.theme)
  const activeTabId = useAppStore((state) => state.activeTabId)
  const toggleTheme = useAppStore((state) => state.toggleTheme)
  const setActiveTab = useAppStore((state) => state.setActiveTab)
  const setSettingsOpen = useAppStore((state) => state.setSettingsOpen)

  return (
    <header className="flex h-14 shrink-0 items-center gap-3 border-b border-line-soft bg-surface/70 px-4 backdrop-blur sm:px-6">
      <div className="flex items-center gap-2.5">
        <Logo size={26} />
        <div className="leading-tight">
          <div className="text-[15px] font-semibold tracking-tight">moLink</div>
          <div className="text-[10px] uppercase tracking-[0.18em] text-muted">
            {t('app.tagline')}
          </div>
        </div>
      </div>

      <div className="ml-auto flex items-center gap-1.5">
        {showHome && activeTabId !== 'home' ? (
          <button
            type="button"
            onClick={() => setActiveTab('home')}
            className="btn btn-quiet"
            title={t('tabs.home')}
          >
            <Home size={15} />
            <span className="hidden sm:inline">{t('tabs.home')}</span>
          </button>
        ) : null}
        <button
          type="button"
          onClick={toggleTheme}
          className="btn btn-quiet h-9 w-9 !px-0"
          title={theme === 'dark' ? t('settings.themeLight') : t('settings.themeDark')}
          aria-label={theme === 'dark' ? t('settings.themeLight') : t('settings.themeDark')}
        >
          {theme === 'dark' ? <Sun size={16} /> : <Moon size={16} />}
        </button>
        <button
          type="button"
          onClick={() => setSettingsOpen(true)}
          className="btn btn-quiet h-9 w-9 !px-0"
          title={t('settings.title')}
          aria-label={t('settings.title')}
        >
          <Settings2 size={16} />
        </button>
      </div>
    </header>
  )
}
