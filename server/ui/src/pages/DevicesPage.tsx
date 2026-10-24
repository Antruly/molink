import { useCallback, useEffect, useState } from 'react'
import { MonitorSmartphone, RefreshCw, Search, Trash2 } from 'lucide-react'
import { ApiError, api, type DevicePage } from '../lib/api'
import { roleRank, type Notify, type Role } from '../lib/types'
import { formatRelative } from '../lib/format'
import {
  Badge,
  Empty,
  ErrorBanner,
  PageHeader,
  Pagination,
  Spinner,
} from '../components/ui'

const PAGE_SIZE = 20

export function DevicesPage({ role, notify }: { role: Role; notify: Notify }) {
  const [page, setPage] = useState(1)
  const [searchInput, setSearchInput] = useState('')
  const [search, setSearch] = useState('')
  const [online, setOnline] = useState('-1')
  const [data, setData] = useState<DevicePage | null>(null)
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    const timer = window.setTimeout(() => {
      setSearch(searchInput.trim())
      setPage(1)
    }, 300)
    return () => window.clearTimeout(timer)
  }, [searchInput])

  const load = useCallback(async () => {
    setLoading(true)
    setError(null)
    try {
      const params = new URLSearchParams({
        page: String(page),
        page_size: String(PAGE_SIZE),
      })
      if (search) params.set('q', search)
      if (online !== '-1') params.set('online', online)
      setData(await api.devices(params))
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : '无法加载设备')
    } finally {
      setLoading(false)
    }
  }, [page, search, online])

  useEffect(() => {
    void load()
  }, [load])

  const revoke = async (id: string, name: string) => {
    if (!window.confirm(`确定吊销设备「${name}」吗？该设备需要重新注册。`)) return
    try {
      await api.revokeDevice(id)
      notify('设备已吊销')
      await load()
    } catch (caught) {
      notify(caught instanceof ApiError ? caught.message : '吊销失败', 'danger')
    }
  }

  return (
    <div>
      <PageHeader
        title="设备"
        description="已注册到目录的客户端设备；心跳超过离线阈值后会显示为离线。"
        actions={
          <button type="button" className="btn h-8 gap-1 px-2 text-[11px]" onClick={() => void load()}>
            <RefreshCw size={13} />
            刷新
          </button>
        }
      />

      <div className="mb-3 flex flex-wrap items-center gap-2">
        <label className="relative flex-1 min-w-[200px] max-w-sm">
          <Search size={14} className="absolute left-2.5 top-1/2 -translate-y-1/2 text-muted" />
          <input
            className="field h-9 pl-8"
            placeholder="搜索名称、ID 或 IP"
            value={searchInput}
            onChange={(event) => setSearchInput(event.target.value)}
          />
        </label>
        <select
          className="field h-9 w-auto [&>option]:bg-surface [&>option]:text-ink"
          value={online}
          onChange={(event) => {
            setOnline(event.target.value)
            setPage(1)
          }}
        >
          <option value="-1">全部状态</option>
          <option value="1">仅在线</option>
          <option value="0">仅离线</option>
        </select>
        {data ? <span className="text-[11px] text-muted">共 {data.total} 台</span> : null}
      </div>

      <ErrorBanner message={error} />
      {loading && !data ? (
        <Spinner />
      ) : !data || data.items.length === 0 ? (
        <Empty>没有符合条件的设备。</Empty>
      ) : (
        <div className="card overflow-hidden p-0">
          <div className="overflow-x-auto">
            <table className="data-table">
              <thead>
                <tr>
                  <th>设备</th>
                  <th>ID</th>
                  <th>地址</th>
                  <th>版本</th>
                  <th>最近心跳</th>
                  <th>状态</th>
                  <th className="text-right">操作</th>
                </tr>
              </thead>
              <tbody>
                {data.items.map((device) => (
                  <tr key={device.id}>
                    <td>
                      <div className="flex items-center gap-2">
                        <span className="grid h-7 w-7 place-items-center rounded-lg bg-panel text-accent">
                          <MonitorSmartphone size={14} />
                        </span>
                        <span className="font-medium text-ink">{device.name}</span>
                      </div>
                    </td>
                    <td className="font-mono text-[11px] text-muted">{device.display_id}</td>
                    <td className="font-mono text-[11px]">
                      <div>{device.address || '—'}</div>
                      <div className="text-muted">
                        {device.peer_ip || '—'}:{device.port || '—'}
                      </div>
                    </td>
                    <td className="font-mono text-[11px] text-muted">{device.version || '—'}</td>
                    <td className="whitespace-nowrap text-muted">
                      {device.last_seen_ms ? formatRelative(device.last_seen_ms) : '—'}
                    </td>
                    <td>
                      <Badge tone={device.online ? 'ok' : 'muted'}>
                        {device.online ? '在线' : '离线'}
                      </Badge>
                    </td>
                    <td className="text-right">
                      {roleRank(role) >= 2 ? (
                        <button
                          type="button"
                          className="btn btn-quiet h-7 gap-1 px-2 text-[11px] hover:!border-danger/40 hover:!text-danger"
                          onClick={() => void revoke(device.id, device.name)}
                        >
                          <Trash2 size={12} />
                          吊销
                        </button>
                      ) : (
                        <span className="text-[10px] text-muted">只读</span>
                      )}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <div className="border-t border-line-soft p-3">
            <Pagination
              page={page}
              pageSize={PAGE_SIZE}
              total={data.total}
              onChange={setPage}
            />
          </div>
        </div>
      )}
    </div>
  )
}
