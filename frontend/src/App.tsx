import { useEffect, useRef } from 'react'
import { useTranslation } from 'react-i18next'
import { AppHeader } from './components/AppHeader'
import { HomeView } from './components/HomeView'
import { SessionView } from './components/SessionView'
import { ResizeFrame } from './components/ResizeFrame'
import { SettingsPanel } from './components/SettingsPanel'
import { TabBar } from './components/TabBar'
import { TitleBar } from './components/TitleBar'
import { useAppStore } from './store/app'

function PopoutFallback() {
  const { t } = useTranslation()
  return (
    <div className="grid flex-1 place-items-center px-6 text-center text-sm text-muted">
      {t('popout.noToken')}
    </div>
  )
}

export default function App() {
  const { t } = useTranslation()
  const theme = useAppStore((state) => state.theme)
  const activeTabId = useAppStore((state) => state.activeTabId)
  const tabs = useAppStore((state) => state.tabs)
  const refreshDevice = useAppStore((state) => state.refreshDevice)
  const createSession = useAppStore((state) => state.createSession)
  const created = useRef(false)

  const popout =
    new URLSearchParams(window.location.search).get('popout') === '1'

  useEffect(() => {
    document.documentElement.dataset.theme = theme
    document
      .querySelector('meta[name="theme-color"]')
      ?.setAttribute('content', theme === 'dark' ? '#0f1719' : '#f4f7f7')
  }, [theme])

  useEffect(() => {
    if (popout) return
    void refreshDevice()
    const timer = window.setInterval(() => {
      if (!document.hidden) void refreshDevice()
    }, 30000)
    return () => window.clearInterval(timer)
  }, [popout, refreshDevice])

  useEffect(() => {
    if (!popout || created.current) return
    created.current = true
    const params = new URLSearchParams(window.location.search)
    const token = params.get('token') ?? ''
    if (token === '') return
    createSession(
      params.get('host') ?? '127.0.0.1',
      Number(params.get('port') ?? '21118'),
      token,
      params.get('label') ?? undefined,
    )
  }, [popout, createSession])

  const active = tabs.find((tab) => tab.id === activeTabId) ?? null
  const showHome = !popout && (activeTabId === 'home' || active === null)

  return (
    <div className="flex h-screen flex-col overflow-hidden bg-canvas text-ink">
      <TitleBar />
      {popout ? null : <AppHeader showHome />}
      {!popout ? <TabBar /> : null}

      <main className="flex min-h-0 flex-1 flex-col">
        {popout ? (
          active ? (
            <SessionView key={active.id} tab={active} popout />
          ) : (
            <PopoutFallback />
          )
        ) : showHome ? (
          <div className="min-h-0 flex-1 overflow-y-auto">
            <HomeView />
          </div>
        ) : active ? (
          <SessionView key={active.id} tab={active} />
        ) : (
          <div className="grid flex-1 place-items-center text-sm text-muted">
            {t('tabs.home')}
          </div>
        )}
      </main>

      <ResizeFrame />
      <SettingsPanel />
    </div>
  )
}
