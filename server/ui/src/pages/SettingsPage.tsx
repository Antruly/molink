import { useCallback, useEffect, useState } from 'react'
import { Copy, Save, Server } from 'lucide-react'
import { ApiError, api, type Overview, type ServerSettings } from '../lib/api'
import { roleRank, type Notify, type Role } from '../lib/types'
import { formatUptime } from '../lib/format'
import {
  Badge,
  Card,
  ErrorBanner,
  Field,
  PageHeader,
  Spinner,
} from '../components/ui'

export function SettingsPage({ role, notify }: { role: Role; notify: Notify }) {
  const [value, setValue] = useState<ServerSettings | null>(null)
  const [overview, setOverview] = useState<Overview | null>(null)
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
  const canEdit = roleRank(role) >= 3

  const load = useCallback(async () => {
    setLoading(true)
    setError(null)
    try {
      const [settings, info] = await Promise.all([
        api.serverSettings(),
        api.overview(),
      ])
      setValue(settings)
      setOverview(info)
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : '无法加载设置')
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => {
    void load()
  }, [load])

  const save = async () => {
    if (!value) return
    setBusy(true)
    setError(null)
    try {
      const saved = await api.saveServerSettings(value)
      setValue(saved)
      notify('服务设置已保存')
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : '保存失败')
    } finally {
      setBusy(false)
    }
  }

  if (loading) return <Spinner />
  if (!value) return <ErrorBanner message={error ?? '无法加载设置'} />

  return (
    <div>
      <PageHeader
        title="设置"
        description="注册、离线判定与公开地址；监听地址与数据库在服务端配置文件中设置。"
        actions={
          <button
            type="button"
            className="btn btn-primary h-8 gap-1 px-2 text-[11px]"
            onClick={() => void save()}
            disabled={!canEdit || busy}
          >
            <Save size={13} />
            {busy ? '保存中…' : '保存设置'}
          </button>
        }
      />
      <ErrorBanner message={error} />

      <div className="mt-3 grid gap-4 xl:grid-cols-2">
        <Card>
          <div className="mb-3 flex items-center gap-2 text-[13px] font-medium text-ink">
            <Server size={15} className="text-accent" />
            服务目录
          </div>
          <div className="space-y-3">
            <Field label="服务名称">
              <input
                className="field"
                value={value.name}
                disabled={!canEdit}
                onChange={(event) => setValue({ ...value, name: event.target.value })}
              />
            </Field>
            <Field label="公开地址" hint="客户端通过设备 ID 连接时展示/解析的地址；留空表示不公开">
              <input
                className="field font-mono text-[11px]"
                value={value.public_url}
                disabled={!canEdit}
                placeholder="https://molink.example.com"
                onChange={(event) => setValue({ ...value, public_url: event.target.value })}
              />
            </Field>
            <Field
              label="注册邀请码"
              hint="留空表示开放注册；设置后客户端注册必须携带同一值"
            >
              <div className="flex gap-2">
                <input
                  className="field font-mono text-[11px]"
                  value={value.registration_token}
                  disabled={!canEdit}
                  onChange={(event) =>
                    setValue({ ...value, registration_token: event.target.value })
                  }
                />
                <button
                  type="button"
                  className="btn h-9 w-9 shrink-0 !px-0"
                  title="复制"
                  disabled={!value.registration_token}
                  onClick={() => {
                    void navigator.clipboard?.writeText(value.registration_token)
                    notify('邀请码已复制')
                  }}
                >
                  <Copy size={14} />
                </button>
              </div>
            </Field>
            <div className="grid grid-cols-2 gap-3">
              <Field label="离线判定（秒）">
                <input
                  className="field h-9"
                  type="number"
                  min={1}
                  value={value.offline_after_seconds}
                  disabled={!canEdit}
                  onChange={(event) =>
                    setValue({
                      ...value,
                      offline_after_seconds: Math.max(1, Number(event.target.value) || 90),
                    })
                  }
                />
              </Field>
              <Field label="设备上限">
                <input
                  className="field h-9"
                  type="number"
                  min={1}
                  value={value.max_devices}
                  disabled={!canEdit}
                  onChange={(event) =>
                    setValue({
                      ...value,
                      max_devices: Math.max(1, Number(event.target.value) || 10000),
                    })
                  }
                />
              </Field>
            </div>
          </div>
        </Card>

        <Card>
          <div className="mb-3 flex items-center gap-2 text-[13px] font-medium text-ink">
            <Server size={15} className="text-accent" />
            运行信息
          </div>
          {overview ? (
            <dl className="space-y-2 text-[12px]">
              <Row label="版本" value={overview.version} mono />
              <Row label="运行时间" value={formatUptime(overview.uptime_ms)} />
              <Row
                label="设备 API"
                value={`${overview.device_api.host}:${overview.device_api.port}`}
                mono
              />
              <Row
                label="管理 API"
                value={`${overview.admin_api.host}:${overview.admin_api.port}`}
                mono
              />
              <Row
                label="数据库"
                value={`${overview.database.driver} · schema ${overview.database.schema_version}`}
                mono
              />
              <Row label="设备" value={`${overview.devices.total} 台 · ${overview.devices.online} 在线`} />
            </dl>
          ) : null}
          <div className="mt-4 rounded-xl border border-line-soft bg-panel/50 p-3 text-[10px] leading-relaxed text-muted">
            <div className="mb-1 flex items-center gap-1 text-[11px] text-ink">
              <Badge tone="warn">需要重启</Badge>
              监听地址、端口、TLS 证书、数据库连接与 UI 目录来自服务端配置文件/命令行；
              修改后需重启服务。此处不提供在线修改，避免管理页把自己锁在外面。
            </div>
          </div>
        </Card>
      </div>
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
