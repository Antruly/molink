import { useEffect, useState } from 'react'
import {
  Activity,
  Ban,
  Database,
  HardDrive,
  MonitorSmartphone,
  Server,
  Users,
} from 'lucide-react'
import { ApiError, api, type AuditEvent, type Overview } from '../lib/api'
import { formatRelative, formatTime, formatUptime } from '../lib/format'
import {
  Badge,
  Card,
  ErrorBanner,
  PageHeader,
  Spinner,
  StatCard,
} from '../components/ui'

export function DashboardPage() {
  const [overview, setOverview] = useState<Overview | null>(null)
  const [events, setEvents] = useState<AuditEvent[]>([])
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    let alive = true
    const load = async () => {
      try {
        const [info, audit] = await Promise.all([
          api.overview(),
          api.audit(new URLSearchParams({ page: '1', page_size: '8' })),
        ])
        if (!alive) return
        setOverview(info)
        setEvents(audit.items)
        setError(null)
      } catch (caught) {
        if (!alive) return
        setError(caught instanceof ApiError ? caught.message : '无法加载概览')
      } finally {
        if (alive) setLoading(false)
      }
    }
    void load()
    const timer = window.setInterval(() => void load(), 10000)
    return () => {
      alive = false
      window.clearInterval(timer)
    }
  }, [])

  return (
    <div>
      <PageHeader
        title="仪表盘"
        description="服务运行状态、设备与安全概况。"
        actions={
          overview ? (
            <Badge tone="ok">
              <Activity size={11} className="mr-1" /> 运行中
            </Badge>
          ) : null
        }
      />
      <ErrorBanner message={error} />
      {loading && !overview ? (
        <Spinner />
      ) : overview ? (
        <div className="space-y-5">
          <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
            <StatCard
              label="在线设备"
              value={overview.devices.online}
              hint={`已注册 ${overview.devices.total} · 离线 ${overview.devices.offline}`}
              icon={<MonitorSmartphone size={18} />}
            />
            <StatCard
              label="管理员账户"
              value={overview.users.total}
              hint="含管理员、操作员与只读账户"
              icon={<Users size={18} />}
            />
            <StatCard
              label="活跃封禁"
              value={overview.security.active_bans}
              hint="临时与永久 IP 封禁"
              icon={<Ban size={18} />}
            />
            <StatCard
              label="运行时间"
              value={formatUptime(overview.uptime_ms)}
              hint={`自 ${formatTime(overview.started_at_ms)}`}
              icon={<Server size={18} />}
            />
          </div>

          <div className="grid gap-4 lg:grid-cols-2">
            <Card>
              <div className="mb-3 flex items-center gap-2 text-[13px] font-medium text-ink">
                <Server size={15} className="text-accent" />
                监听与版本
              </div>
              <dl className="space-y-2 text-[12px]">
                <Row label="服务版本" value={overview.version} mono />
                <Row
                  label="设备 API"
                  value={`${overview.device_api.host}:${overview.device_api.port}${overview.device_api.tls ? ' · TLS' : ''}`}
                  mono
                />
                <Row
                  label="管理 API"
                  value={`${overview.admin_api.host}:${overview.admin_api.port}${overview.admin_api.tls ? ' · TLS' : ''}`}
                  mono
                />
                <Row
                  label="数据库"
                  value={`${overview.database.driver} · schema ${overview.database.schema_version}`}
                  mono
                />
              </dl>
            </Card>

            <Card>
              <div className="mb-3 flex items-center gap-2 text-[13px] font-medium text-ink">
                <Database size={15} className="text-accent" />
                运行提示
              </div>
              <ul className="space-y-2 text-[11px] leading-relaxed text-muted">
                <li className="flex gap-2">
                  <HardDrive size={13} className="mt-0.5 shrink-0 text-accent" />
                  默认只允许私有网段与中国大陆地址访问设备 API；可在「安全策略」调整。
                </li>
                <li className="flex gap-2">
                  <HardDrive size={13} className="mt-0.5 shrink-0 text-accent" />
                  建议在公网部署时为管理端口配置 TLS 或放在 HTTPS 反向代理之后。
                </li>
                <li className="flex gap-2">
                  <HardDrive size={13} className="mt-0.5 shrink-0 text-accent" />
                  设备心跳间隔 30 秒；离线判定时间可在「设置」中调整。
                </li>
              </ul>
            </Card>
          </div>

          <Card>
            <div className="mb-3 flex items-center justify-between gap-3">
              <div className="flex items-center gap-2 text-[13px] font-medium text-ink">
                <Activity size={15} className="text-accent" />
                最近事件
              </div>
              <span className="text-[10px] text-muted">审计保留 90 天</span>
            </div>
            {events.length === 0 ? (
              <p className="py-4 text-center text-[11px] text-muted">暂无事件</p>
            ) : (
              <div className="overflow-x-auto">
                <table className="data-table">
                  <thead>
                    <tr>
                      <th>时间</th>
                      <th>事件</th>
                      <th>操作者</th>
                      <th>来源</th>
                      <th>对象</th>
                    </tr>
                  </thead>
                  <tbody>
                    {events.map((event) => (
                      <tr key={event.id}>
                        <td className="whitespace-nowrap text-muted">
                          {formatRelative(event.ts_ms)}
                        </td>
                        <td className="font-mono text-[11px] text-ink">{event.event}</td>
                        <td>{event.actor_name || '—'}</td>
                        <td className="font-mono text-[11px] text-muted">
                          {event.ip || '—'}
                        </td>
                        <td className="font-mono text-[11px] text-muted">
                          {event.target || '—'}
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            )}
          </Card>
        </div>
      ) : null}
    </div>
  )
}

function Row({ label, value, mono = false }: { label: string; value: string; mono?: boolean }) {
  return (
    <div className="flex items-center justify-between gap-3">
      <dt className="text-muted">{label}</dt>
      <dd className={mono ? 'font-mono text-[11px] text-ink' : 'text-ink'}>{value}</dd>
    </div>
  )
}
