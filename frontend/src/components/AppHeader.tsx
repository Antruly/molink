import { Moon, Settings2, Sun } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { useAppStore } from '../store/app'
import { Logo } from './Logo'
import { TabBar } from './TabBar'
import { UpdateNotice } from './UpdateNotice'

export function AppHeader({ showHome = true }: { showHome?: boolean }) {
  const { t } = useTranslation()
  const theme = useAppStore((state) => state.theme)
  const activeTabId = useAppStore((state) => state.activeTabId)
  const toggleTheme = useAppStore((state) => state.toggleTheme)
  const setSettingsOpen = useAppStore((state) => state.setSettingsOpen)
  // Inside a session the floating toolbar owns theme and settings; the home
  // page keeps them here so they remain reachable without a session.
  const home = activeTabId === 'home' || !showHome

  return (
    <header className="flex h-12 shrink-0 items-center gap-2 border-b border-line-soft bg-surface/70 px-3 backdrop-blur sm:px-4">
      <div className="flex shrink-0 items-center gap-2">
        <Logo size={24} />
        <span className="text-[15px] font-semibold tracking-tight">moLink</span>
      </div>
      <div className="mx-1 h-5 w-px shrink-0 bg-line" />
      <TabBar inline />
      <div className="ml-1 flex shrink-0 items-center gap-1">
        <UpdateNotice />
        {home ? (
          <>
            <button
              type="button"
              onClick={toggleTheme}
              className="btn btn-quiet h-8 w-8 !px-0"
              title={theme === 'dark' ? t('settings.themeLight') : t('settings.themeDark')}
              aria-label={theme === 'dark' ? t('settings.themeLight') : t('settings.themeDark')}
            >
              {theme === 'dark' ? <Sun size={15} /> : <Moon size={15} />}
            </button>
            <button
              type="button"
              onClick={() => setSettingsOpen(true)}
              className="btn btn-quiet h-8 w-8 !px-0"
              title={t('settings.title')}
              aria-label={t('settings.title')}
            >
              <Settings2 size={15} />
            </button>
          </>
        ) : null}
      </div>
    </header>
  )
}
