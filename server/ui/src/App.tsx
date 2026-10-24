import { useCallback, useEffect, useState } from 'react'
import {
  Ban,
  LayoutDashboard,
  LogOut,
  Menu,
  MonitorSmartphone,
  Moon,
  ScrollText,
  Server,
  Settings as SettingsIcon,
  ShieldCheck,
  Sun,
  Users,
  X,
} from 'lucide-react'
import { ApiError, api, type SessionInfo } from './lib/api'
import { roleRank, type Notify, type Role } from './lib/types'
import { SetupPage } from './pages/SetupPage'
import { LoginPage } from './pages/LoginPage'
import { DashboardPage } from './pages/DashboardPage'
import { DevicesPage } from './pages/DevicesPage'
import { UsersPage } from './pages/UsersPage'
import { SecurityPage } from './pages/SecurityPage'
import { BansPage } from './pages/BansPage'
import { AuditPage } from './pages/AuditPage'
import { SettingsPage } from './pages/SettingsPage'
import { Spinner } from './components/ui'

type Page =
  | 'dashboard'
  | 'devices'
  | 'users'
  | 'security'
  | 'bans'
  | 'audit'
  | 'settings'
type Theme = 'light' | 'dark'

const NAV: Array<{
  page: Page
  label: string
  icon: typeof LayoutDashboard
  rank: number
}> = [
  { page: 'dashboard', label: '仪表盘', icon: LayoutDashboard, rank: 1 },
  { page: 'devices', label: '设备', icon: MonitorSmartphone, rank: 1 },
  { page: 'users', label: '管理员', icon: Users, rank: 3 },
  { page: 'security', label: '安全策略', icon: ShieldCheck, rank: 2 },
  { page: 'bans', label: '封禁', icon: Ban, rank: 2 },
  { page: 'audit', label: '审计', icon: ScrollText, rank: 2 },
  { page: 'settings', label: '设置', icon: SettingsIcon, rank: 1 },
]

function initialTheme(): Theme {
  try {
    const saved = window.localStorage.getItem('molink.admin.theme')
    if (saved === 'light' || saved === 'dark') return saved
  } catch {
    // Storage can be blocked.
  }
  return window.matchMedia?.('(prefers-color-scheme: light)').matches
    ? 'light'
    : 'dark'
}

export default function App() {
  const [session, setSession] = useState<SessionInfo | null>(null)
  const [loading, setLoading] = useState(true)
  const [loadError, setLoadError] = useState<string | null>(null)
  const [page, setPage] = useState<Page>('dashboard')
  const [theme, setTheme] = useState<Theme>(initialTheme)
  const [toast, setToast] = useState<{ message: string; tone: 'ok' | 'danger' } | null>(
    null,
  )
  const [navOpen, setNavOpen] = useState(false)

  const notify: Notify = useCallback((message, tone = 'ok') => {
    setToast({ message, tone })
  }, [])

  useEffect(() => {
    if (!toast) return
    const timer = window.setTimeout(() => setToast(null), 3200)
    return () => window.clearTimeout(timer)
  }, [toast])

  useEffect(() => {
    document.documentElement.dataset.theme = theme
    try {
      window.localStorage.setItem('molink.admin.theme', theme)
    } catch {
      // Storage can be blocked.
    }
  }, [theme])

  const refresh = useCallback(async () => {
    setLoading(true)
    setLoadError(null)
    try {
      const info = await api.session()
      setSession(info)
    } catch (error) {
      setLoadError(
        error instanceof ApiError ? error.message : '无法连接到 moLink Server',
      )
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => {
    void refresh()
  }, [refresh])

  const onAuthed = (result: {
    user: SessionInfo['user']
    csrf_token: string
    expires_at_ms: number
  }) => {
    setSession({
      authenticated: true,
      needs_setup: false,
      user: result.user,
      csrf_token: result.csrf_token,
      expires_at_ms: result.expires_at_ms,
    })
    setPage('dashboard')
  }

  const logout = async () => {
    try {
      await api.logout()
    } catch {
      // The session may already be gone.
    }
    setSession({ authenticated: false, needs_setup: false })
    setPage('dashboard')
  }

  if (loading) {
    return (
      <div className="grid min-h-screen place-items-center">
        <Spinner label="正在连接 moLink Server…" />
      </div>
    )
  }

  if (loadError && !session) {
    return (
      <div className="grid min-h-screen place-items-center p-6">
        <div className="card max-w-sm text-center">
          <Server size={28} className="mx-auto mb-3 text-accent" />
          <div className="text-[15px] font-semibold text-ink">无法连接服务端</div>
          <p className="mt-2 text-[12px] leading-relaxed text-muted">{loadError}</p>
          <button type="button" className="btn btn-primary mt-4" onClick={() => void refresh()}>
            重试
          </button>
        </div>
      </div>
    )
  }

  if (session?.needs_setup) {
    return <SetupPage onAuthed={onAuthed} />
  }

  if (!session?.authenticated || !session.user) {
    return <LoginPage onAuthed={onAuthed} />
  }

  const user = session.user
  const rank = roleRank(user.role)
  const items = NAV.filter((item) => rank >= item.rank)

  return (
    <div className="min-h-screen">
      <div className="admin-shell">
        <aside
          className={
            'admin-nav z-40 ' +
            (navOpen
              ? 'fixed inset-y-0 left-0 block shadow-2xl md:static md:shadow-none'
              : 'hidden md:block')
          }
        >
          <div className="mb-6 flex items-center gap-2 px-1">
            <span className="grid h-9 w-9 place-items-center rounded-xl bg-accent-soft text-accent">
              <Server size={18} />
            </span>
            <div className="min-w-0">
              <div className="text-[14px] font-semibold text-ink">moLink Server</div>
              <div className="text-[10px] text-muted">管理控制台</div>
            </div>
            <button
              type="button"
              className="ml-auto grid h-8 w-8 place-items-center rounded-lg text-muted hover:bg-panel md:hidden"
              onClick={() => setNavOpen(false)}
              aria-label="关闭菜单"
            >
              <X size={16} />
            </button>
          </div>
          <nav className="space-y-1">
            {items.map((item) => (
              <button
                key={item.page}
                type="button"
                data-active={page === item.page}
                className="admin-nav-item"
                onClick={() => {
                  setPage(item.page)
                  setNavOpen(false)
                }}
              >
                <item.icon size={16} />
                {item.label}
              </button>
            ))}
          </nav>
          <div className="mt-6 rounded-xl border border-line-soft bg-panel/50 p-3 text-[10px] leading-relaxed text-muted">
            <div className="flex items-center justify-between">
              <span>版本</span>
              <span className="font-mono text-ink">{session.version ?? '0.2.0'}</span>
            </div>
            <div className="mt-1 flex items-center justify-between">
              <span>角色</span>
              <span className="text-accent">{user.role}</span>
            </div>
          </div>
        </aside>

        {navOpen ? (
          <button
            type="button"
            className="fixed inset-0 z-30 bg-black/40 md:hidden"
            onClick={() => setNavOpen(false)}
            aria-label="关闭菜单"
          />
        ) : null}

        <div className="flex min-w-0 flex-1 flex-col">
          <header className="sticky top-0 z-20 flex h-14 items-center gap-3 border-b border-line-soft bg-canvas/85 px-3 backdrop-blur sm:px-5">
            <button
              type="button"
              className="grid h-8 w-8 place-items-center rounded-lg text-muted hover:bg-panel md:hidden"
              onClick={() => setNavOpen(true)}
              aria-label="打开菜单"
            >
              <Menu size={17} />
            </button>
            <div className="min-w-0">
              <div className="truncate text-[13px] font-medium text-ink">
                {NAV.find((item) => item.page === page)?.label}
              </div>
              <div className="text-[10px] text-muted">
                {user.display_name || user.username} · {user.role}
              </div>
            </div>
            <div className="ml-auto flex items-center gap-1.5">
              <button
                type="button"
                className="btn btn-quiet h-8 w-8 !px-0"
                onClick={() => setTheme(theme === 'dark' ? 'light' : 'dark')}
                title="切换主题"
                aria-label="切换主题"
              >
                {theme === 'dark' ? <Sun size={15} /> : <Moon size={15} />}
              </button>
              <button
                type="button"
                className="btn btn-quiet h-8 gap-1 px-2 text-[11px]"
                onClick={() => void logout()}
              >
                <LogOut size={14} />
                <span className="hidden sm:inline">退出</span>
              </button>
            </div>
          </header>

          <main className="min-h-0 flex-1 overflow-y-auto p-4 sm:p-6">
            {page === 'dashboard' ? <DashboardPage /> : null}
            {page === 'devices' ? (
              <DevicesPage role={user.role as Role} notify={notify} />
            ) : null}
            {page === 'users' ? <UsersPage role={user.role as Role} notify={notify} /> : null}
            {page === 'security' ? (
              <SecurityPage role={user.role as Role} notify={notify} />
            ) : null}
            {page === 'bans' ? <BansPage role={user.role as Role} notify={notify} /> : null}
            {page === 'audit' ? <AuditPage /> : null}
            {page === 'settings' ? (
              <SettingsPage role={user.role as Role} notify={notify} />
            ) : null}
          </main>
        </div>
      </div>

      {toast ? (
        <div
          className={
            'fixed bottom-4 right-4 z-[60] rounded-xl border px-4 py-2.5 text-[12px] shadow-2xl backdrop-blur ' +
            (toast.tone === 'danger'
              ? 'border-danger/40 bg-danger/15 text-danger'
              : 'border-accent/40 bg-accent-soft text-accent')
          }
        >
          {toast.message}
        </div>
      ) : null}
    </div>
  )
}
