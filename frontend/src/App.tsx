import { Loader2 } from 'lucide-react'
import { useEffect, useRef } from 'react'
import { useTranslation } from 'react-i18next'
import { AppHeader } from './components/AppHeader'
import { AuthGate } from './components/AuthGate'
import { HomeView } from './components/HomeView'
import { Logo } from './components/Logo'
import { AccessRequestDialog } from './components/AccessRequestDialog'
import { ResizeFrame } from './components/ResizeFrame'
import { SessionView } from './components/SessionView'
import { SettingsPanel } from './components/SettingsPanel'
import { TabBar } from './components/TabBar'
import { TitleBar } from './components/TitleBar'
import { useAppStore } from './store/app'

function BootSplash() {
  return (
    <div className="flex flex-1 items-center justify-center">
      <div className="flex items-center gap-3 text-muted">
        <Logo size={38} />
        <Loader2 size={18} className="animate-spin text-accent" />
      </div>
    </div>
  )
}

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
  const authPhase = useAppStore((state) => state.authPhase)
  const activeTabId = useAppStore((state) => state.activeTabId)
  const tabs = useAppStore((state) => state.tabs)
  const boot = useAppStore((state) => state.boot)
  const refreshState = useAppStore((state) => state.refreshState)
  const touchActivity = useAppStore((state) => state.touchActivity)
  const createSession = useAppStore((state) => state.createSession)
  const created = useRef(false)

  const popout =
    new URLSearchParams(window.location.search).get('popout') === '1'

  useEffect(() => {
    document.documentElement.dataset.theme = theme
    document
      .querySelector('meta[name="theme-color"]')
      ?.setAttribute('content', theme === 'dark' ? '#0b1012' : '#f4f7f7')
  }, [theme])

  useEffect(() => {
    void boot()
  }, [boot])

  useEffect(() => {
    const timer = window.setInterval(() => {
      if (!document.hidden) void refreshState()
    }, 3000)
    return () => window.clearInterval(timer)
  }, [refreshState])

  useEffect(() => {
    if (authPhase !== 'ready') return
    const timer = window.setInterval(() => {
      const state = useAppStore.getState()
      if (
        state.tabs.some(
          (tab) => tab.phase === 'live' || tab.phase === 'connecting',
        )
      ) {
        state.syncUiState()
      }
    }, 20000)
    return () => window.clearInterval(timer)
  }, [authPhase])

  useEffect(() => {
    if (authPhase !== 'ready') return
    let last = 0
    const onActivity = () => {
      const now = Date.now()
      if (now - last < 15000) return
      last = now
      touchActivity()
    }
    window.addEventListener('pointerdown', onActivity)
    window.addEventListener('keydown', onActivity)
    window.addEventListener('wheel', onActivity, { passive: true })
    return () => {
      window.removeEventListener('pointerdown', onActivity)
      window.removeEventListener('keydown', onActivity)
      window.removeEventListener('wheel', onActivity)
    }
  }, [authPhase, touchActivity])

  useEffect(() => {
    if (!popout || authPhase !== 'ready' || created.current) return
    created.current = true
    const params = new URLSearchParams(window.location.search)
    const token = params.get('token') ?? ''
    // An empty token means "ask the host for approval" rather than a hard
    // failure; the core decides whether that flow is enabled.
    createSession(
      params.get('host') ?? '127.0.0.1',
      Number(params.get('port') ?? '28418'),
      token,
      params.get('label') ?? undefined,
      false,
      token === '',
    )
  }, [popout, authPhase, createSession])

  const ready = authPhase === 'ready'
  const active = tabs.find((tab) => tab.id === activeTabId) ?? null
  const showHome = !popout && (activeTabId === 'home' || active === null)

  return (
    <div className="flex h-screen flex-col overflow-hidden bg-canvas text-ink">
      <TitleBar />
      <ResizeFrame />
      <AccessRequestDialog />

      {authPhase === 'booting' ? (
        <BootSplash />
      ) : !ready ? (
        <AuthGate />
      ) : (
        <>
          {popout ? null : <AppHeader showHome />}
          {popout ? null : <TabBar />}
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
          <SettingsPanel />
        </>
      )}
    </div>
  )
}
