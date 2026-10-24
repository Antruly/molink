import { useCallback, useEffect, useState } from 'react'
import { ScrollText } from 'lucide-react'
import { ApiError, api, type AuditPage as AuditData } from '../lib/api'
import { formatTime } from '../lib/format'
import {
  Badge,
  Empty,
  ErrorBanner,
  PageHeader,
  Pagination,
  Spinner,
} from '../components/ui'

const PAGE_SIZE = 30

export function AuditPage() {
  const [page, setPage] = useState(1)
  const [data, setData] = useState<AuditData | null>(null)
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)

  const load = useCallback(async () => {
    setLoading(true)
    setError(null)
    try {
      setData(
        await api.audit(
          new URLSearchParams({ page: String(page), page_size: String(PAGE_SIZE) }),
        ),
      )
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : '无法加载审计事件')
    } finally {
      setLoading(false)
    }
  }, [page])

  useEffect(() => {
    void load()
  }, [load])

  return (
    <div>
      <PageHeader
        title="审计"
        description="登录、账户、封禁与策略变更记录，默认保留 90 天。"
      />
      <ErrorBanner message={error} />
      {loading && !data ? (
        <Spinner />
      ) : !data || data.items.length === 0 ? (
        <Empty>
          <ScrollText size={22} className="mx-auto mb-2 text-muted" />
          暂无审计事件。
        </Empty>
      ) : (
        <div className="card overflow-hidden p-0">
          <div className="overflow-x-auto">
            <table className="data-table">
              <thead>
                <tr>
                  <th>时间</th>
                  <th>事件</th>
                  <th>结果</th>
                  <th>操作者</th>
                  <th>角色</th>
                  <th>来源 IP</th>
                  <th>对象</th>
                </tr>
              </thead>
              <tbody>
                {data.items.map((event) => (
                  <tr key={event.id}>
                    <td className="whitespace-nowrap text-muted">{formatTime(event.ts_ms)}</td>
                    <td className="font-mono text-[11px] text-ink">{event.event}</td>
                    <td>
                      <Badge tone={event.result === 'ok' ? 'ok' : 'danger'}>
                        {event.result}
                      </Badge>
                    </td>
                    <td>{event.actor_name || '—'}</td>
                    <td className="text-muted">{event.role || '—'}</td>
                    <td className="font-mono text-[11px] text-muted">{event.ip || '—'}</td>
                    <td className="font-mono text-[11px] text-muted">{event.target || '—'}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <div className="border-t border-line-soft p-3">
            <Pagination page={page} pageSize={PAGE_SIZE} total={data.total} onChange={setPage} />
          </div>
        </div>
      )}
    </div>
  )
}
