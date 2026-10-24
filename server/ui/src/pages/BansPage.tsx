import { useCallback, useEffect, useState, type FormEvent } from 'react'
import { Ban, Plus, Trash2 } from 'lucide-react'
import { ApiError, api, type BanPage } from '../lib/api'
import { roleRank, type Notify, type Role } from '../lib/types'
import { formatDuration, formatTime } from '../lib/format'
import {
  Badge,
  Empty,
  ErrorBanner,
  Field,
  Modal,
  PageHeader,
  Pagination,
  Spinner,
} from '../components/ui'

const PAGE_SIZE = 20

export function BansPage({ role, notify }: { role: Role; notify: Notify }) {
  const [page, setPage] = useState(1)
  const [data, setData] = useState<BanPage | null>(null)
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)
  const [open, setOpen] = useState(false)
  const [target, setTarget] = useState('')
  const [reason, setReason] = useState('manual')
  const [duration, setDuration] = useState(3600)
  const [note, setNote] = useState('')
  const [busy, setBusy] = useState(false)
  const [formError, setFormError] = useState<string | null>(null)
  const isAdmin = roleRank(role) >= 3

  const load = useCallback(async () => {
    setLoading(true)
    setError(null)
    try {
      setData(
        await api.bans(
          new URLSearchParams({ page: String(page), page_size: String(PAGE_SIZE) }),
        ),
      )
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : '无法加载封禁列表')
    } finally {
      setLoading(false)
    }
  }, [page])

  useEffect(() => {
    void load()
  }, [load])

  const submit = async (event: FormEvent) => {
    event.preventDefault()
    setBusy(true)
    setFormError(null)
    try {
      await api.addBan({
        target: target.trim(),
        reason: reason.trim() || 'manual',
        duration_seconds: duration,
        note: note.trim(),
      })
      notify('封禁已添加')
      setOpen(false)
      setTarget('')
      setNote('')
      await load()
    } catch (caught) {
      setFormError(caught instanceof ApiError ? caught.message : '添加失败')
    } finally {
      setBusy(false)
    }
  }

  const remove = async (id: string, targetValue: string) => {
    if (!window.confirm(`解除对 ${targetValue} 的封禁？`)) return
    try {
      await api.deleteBan(id)
      notify('封禁已解除')
      await load()
    } catch (caught) {
      notify(caught instanceof ApiError ? caught.message : '解除失败', 'danger')
    }
  }

  return (
    <div>
      <PageHeader
        title="封禁"
        description="临时或永久拒绝某个 IP / CIDR 访问设备 API 与管理 API。"
        actions={
          <button
            type="button"
            className="btn btn-primary h-8 gap-1 px-2 text-[11px]"
            onClick={() => {
              setFormError(null)
              setOpen(true)
            }}
          >
            <Plus size={13} />
            添加封禁
          </button>
        }
      />
      <ErrorBanner message={error} />
      {loading && !data ? (
        <Spinner />
      ) : !data || data.items.length === 0 ? (
        <Empty>当前没有封禁记录。</Empty>
      ) : (
        <div className="card overflow-hidden p-0">
          <div className="overflow-x-auto">
            <table className="data-table">
              <thead>
                <tr>
                  <th>目标</th>
                  <th>类型</th>
                  <th>原因</th>
                  <th>来源</th>
                  <th>到期</th>
                  <th>创建者</th>
                  <th className="text-right">操作</th>
                </tr>
              </thead>
              <tbody>
                {data.items.map((ban) => (
                  <tr key={ban.id}>
                    <td className="font-mono text-[11px] text-ink">{ban.target}</td>
                    <td>
                      <Badge tone="muted">{ban.kind}</Badge>
                    </td>
                    <td>{ban.reason || '—'}</td>
                    <td>
                      <Badge tone={ban.source === 'auto' ? 'warn' : 'muted'}>
                        {ban.source === 'auto' ? '自动' : '手动'}
                      </Badge>
                    </td>
                    <td className="whitespace-nowrap text-muted">
                      {ban.expires_ms === 0 ? '永久' : formatTime(ban.expires_ms)}
                      {ban.expires_ms !== 0 ? (
                        <div className="text-[10px]">
                          {formatDuration(Math.max(0, (ban.expires_ms - Date.now()) / 1000))}后
                        </div>
                      ) : null}
                    </td>
                    <td className="text-muted">{ban.created_by || '—'}</td>
                    <td className="text-right">
                      <button
                        type="button"
                        className="btn btn-quiet h-7 gap-1 px-2 text-[11px]"
                        onClick={() => void remove(ban.id, ban.target)}
                      >
                        <Trash2 size={12} />
                        解除
                      </button>
                    </td>
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

      <Modal
        open={open}
        title="添加封禁"
        onClose={() => setOpen(false)}
        footer={
          <>
            <button type="button" className="btn h-8" onClick={() => setOpen(false)}>
              取消
            </button>
            <button type="submit" form="ban-form" className="btn btn-primary h-8" disabled={busy}>
              {busy ? '添加中…' : '添加封禁'}
            </button>
          </>
        }
      >
        <form id="ban-form" className="space-y-3" onSubmit={submit}>
          <Field label="目标" hint="单个 IP 或 CIDR，例如 198.51.100.7 或 198.51.100.0/24">
            <input
              className="field font-mono"
              value={target}
              onChange={(event) => setTarget(event.target.value)}
              required
            />
          </Field>
          <Field label="原因">
            <input className="field" value={reason} onChange={(event) => setReason(event.target.value)} />
          </Field>
          <Field label="时长">
            <select
              className="field [&>option]:bg-surface [&>option]:text-ink"
              value={duration}
              onChange={(event) => setDuration(Number(event.target.value))}
            >
              <option value={3600}>1 小时</option>
              <option value={21600}>6 小时</option>
              <option value={86400}>1 天</option>
              <option value={604800}>7 天</option>
              {isAdmin ? <option value={0}>永久（仅管理员）</option> : null}
            </select>
          </Field>
          <Field label="备注（可选）">
            <input className="field" value={note} onChange={(event) => setNote(event.target.value)} />
          </Field>
          <ErrorBanner message={formError} />
          <div className="flex items-center gap-2 rounded-lg border border-line-soft bg-panel/50 px-3 py-2 text-[10px] text-muted">
            <Ban size={13} className="text-warn" />
            封禁同时作用于设备 API 与管理 API；回环地址不受影响。
          </div>
        </form>
      </Modal>
    </div>
  )
}
