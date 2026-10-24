import { useCallback, useEffect, useState, type ReactNode } from 'react'
import { Save, ShieldCheck } from 'lucide-react'
import { ApiError, api, type SecuritySettings } from '../lib/api'
import { roleRank, type Notify, type Role } from '../lib/types'
import {
  Badge,
  Card,
  ErrorBanner,
  Field,
  PageHeader,
  Spinner,
  Switch,
} from '../components/ui'

function lines(value: string): string[] {
  return value
    .split('\n')
    .map((line) => line.trim())
    .filter(Boolean)
}

export function SecurityPage({ role, notify }: { role: Role; notify: Notify }) {
  const [value, setValue] = useState<SecuritySettings | null>(null)
  const [allowText, setAllowText] = useState('')
  const [denyText, setDenyText] = useState('')
  const [proxyText, setProxyText] = useState('')
  const [ladderText, setLadderText] = useState('')
  const [loading, setLoading] = useState(true)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [loadError, setLoadError] = useState<string | null>(null)
  const canEdit = roleRank(role) >= 3

  const load = useCallback(async () => {
    setLoading(true)
    setLoadError(null)
    try {
      const document = await api.security()
      setValue(document)
      setAllowText(document.ip_policy.custom_allow.join('\n'))
      setDenyText(document.ip_policy.custom_deny.join('\n'))
      setProxyText(document.trusted_proxies.join('\n'))
      setLadderText(document.login.ban_ladder_seconds.join(', '))
    } catch (caught) {
      setLoadError(caught instanceof ApiError ? caught.message : '无法加载安全策略')
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => {
    void load()
  }, [load])

  const patch = (updater: (current: SecuritySettings) => SecuritySettings) => {
    setValue((current) => (current ? updater(current) : current))
  }

  const save = async () => {
    if (!value) return
    const ladder = ladderText
      .split(',')
      .map((item) => Number(item.trim()))
      .filter((item) => Number.isFinite(item) && item > 0)
    if (ladder.length === 0) {
      setError('封禁阶梯至少需要一个正整数秒数')
      return
    }
    setBusy(true)
    setError(null)
    try {
      const payload: SecuritySettings = {
        ...value,
        ip_policy: {
          ...value.ip_policy,
          custom_allow: lines(allowText),
          custom_deny: lines(denyText),
        },
        trusted_proxies: lines(proxyText),
        login: { ...value.login, ban_ladder_seconds: ladder },
      }
      const saved = await api.saveSecurity(payload)
      setValue(saved)
      notify('安全策略已保存并立即生效')
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : '保存失败')
    } finally {
      setBusy(false)
    }
  }

  if (loading) return <Spinner />
  if (loadError || !value) {
    return <ErrorBanner message={loadError ?? '无法加载安全策略'} />
  }

  const updateLimit = (key: string, number: number) =>
    patch((current) => ({
      ...current,
      limits: { ...current.limits, [key]: number },
    }))

  return (
    <div>
      <PageHeader
        title="安全策略"
        description="来源 IP 范围、请求限速、登录保护与会话有效期。保存后立即生效。"
        actions={
          <>
            {canEdit ? null : <Badge tone="warn">只读：只有管理员可以修改</Badge>}
            <button
              type="button"
              className="btn btn-primary h-8 gap-1 px-2 text-[11px]"
              onClick={() => void save()}
              disabled={!canEdit || busy}
            >
              <Save size={13} />
              {busy ? '保存中…' : '保存策略'}
            </button>
          </>
        }
      />

      <ErrorBanner message={error} />

      <div className="mt-3 grid gap-4 xl:grid-cols-2">
        <Card>
          <Section title="IP 来源策略" hint="默认仅允许私有网段与中国大陆地址">
            <ToggleRow
              label="允许私有网段"
              hint="10/8、172.16/12、192.168/16、回环、链路本地、CGNAT 等"
              checked={value.ip_policy.allow_private}
              disabled={!canEdit}
              onChange={(checked) =>
                patch((current) => ({
                  ...current,
                  ip_policy: { ...current.ip_policy, allow_private: checked },
                }))
              }
            />
            <ToggleRow
              label="允许中国大陆"
              hint="使用随包发布的 APNIC 中国 IPv4 段"
              checked={value.ip_policy.allow_mainland_china}
              disabled={!canEdit}
              onChange={(checked) =>
                patch((current) => ({
                  ...current,
                  ip_policy: { ...current.ip_policy, allow_mainland_china: checked },
                }))
              }
            />
            <ToggleRow
              label="允许海外地址"
              hint="关闭时，除自定义允许列表外的海外来源会被拒绝"
              checked={value.ip_policy.allow_overseas}
              disabled={!canEdit}
              onChange={(checked) =>
                patch((current) => ({
                  ...current,
                  ip_policy: { ...current.ip_policy, allow_overseas: checked },
                }))
              }
            />
            <Field label="自定义拒绝（每行一个 CIDR，优先级最高）">
              <textarea
                className="field min-h-[76px] font-mono text-[11px]"
                value={denyText}
                onChange={(event) => setDenyText(event.target.value)}
                disabled={!canEdit}
                placeholder="198.51.100.0/24"
              />
            </Field>
            <Field label="自定义允许（每行一个 CIDR）">
              <textarea
                className="field min-h-[76px] font-mono text-[11px]"
                value={allowText}
                onChange={(event) => setAllowText(event.target.value)}
                disabled={!canEdit}
                placeholder="203.0.113.7/32"
              />
            </Field>
            <Field
              label="受信代理（每行一个 CIDR）"
              hint="只有来自这些地址的 X-Forwarded-For 才会被采用；留空则不信任任何代理头"
            >
              <textarea
                className="field min-h-[64px] font-mono text-[11px]"
                value={proxyText}
                onChange={(event) => setProxyText(event.target.value)}
                disabled={!canEdit}
                placeholder="127.0.0.1/32"
              />
            </Field>
          </Section>
        </Card>

        <Card>
          <Section title="请求限速" hint="按来源 IP、设备或会话计数的令牌桶">
            <div className="grid grid-cols-2 gap-3">
              <NumberInput
                label="设备 API 每分钟"
                value={value.limits.device_ip_requests_per_minute ?? 600}
                disabled={!canEdit}
                onChange={(n) => updateLimit('device_ip_requests_per_minute', n)}
              />
              <NumberInput
                label="设备 API 突发"
                value={value.limits.device_ip_burst ?? 120}
                disabled={!canEdit}
                onChange={(n) => updateLimit('device_ip_burst', n)}
              />
              <NumberInput
                label="注册 / 小时 / IP"
                value={value.limits.register_per_ip_per_hour ?? 10}
                disabled={!canEdit}
                onChange={(n) => updateLimit('register_per_ip_per_hour', n)}
              />
              <NumberInput
                label="每 IP 设备上限"
                value={value.limits.devices_per_ip ?? 50}
                disabled={!canEdit}
                onChange={(n) => updateLimit('devices_per_ip', n)}
              />
              <NumberInput
                label="心跳 / 分钟 / 设备"
                value={value.limits.heartbeat_per_device_per_minute ?? 6}
                disabled={!canEdit}
                onChange={(n) => updateLimit('heartbeat_per_device_per_minute', n)}
              />
              <NumberInput
                label="心跳 / 分钟 / IP"
                value={value.limits.heartbeat_per_ip_per_minute ?? 240}
                disabled={!canEdit}
                onChange={(n) => updateLimit('heartbeat_per_ip_per_minute', n)}
              />
              <NumberInput
                label="查询 / 分钟 / IP"
                value={value.limits.lookup_per_ip_per_minute ?? 60}
                disabled={!canEdit}
                onChange={(n) => updateLimit('lookup_per_ip_per_minute', n)}
              />
              <NumberInput
                label="管理 API / 分钟 / 会话"
                value={value.limits.admin_api_per_session_per_minute ?? 300}
                disabled={!canEdit}
                onChange={(n) => updateLimit('admin_api_per_session_per_minute', n)}
              />
              <NumberInput
                label="登录尝试 / 5 分钟 / IP"
                value={value.limits.admin_login_per_ip_per_5min ?? 10}
                disabled={!canEdit}
                onChange={(n) => updateLimit('admin_login_per_ip_per_5min', n)}
              />
              <NumberInput
                label="注销 / 分钟 / IP"
                value={value.limits.unregister_per_ip_per_minute ?? 10}
                disabled={!canEdit}
                onChange={(n) => updateLimit('unregister_per_ip_per_minute', n)}
              />
            </div>
          </Section>
        </Card>

        <Card>
          <Section title="登录保护" hint="连续失败后进入临时封禁阶梯">
            <div className="grid grid-cols-2 gap-3">
              <NumberInput
                label="失败次数阈值"
                value={value.login.failure_limit}
                disabled={!canEdit}
                onChange={(n) =>
                  patch((current) => ({
                    ...current,
                    login: { ...current.login, failure_limit: n },
                  }))
                }
              />
              <NumberInput
                label="统计窗口（秒）"
                value={value.login.failure_window_seconds}
                disabled={!canEdit}
                onChange={(n) =>
                  patch((current) => ({
                    ...current,
                    login: { ...current.login, failure_window_seconds: n },
                  }))
                }
              />
              <NumberInput
                label="阶梯重置（秒）"
                value={value.login.strike_reset_seconds}
                disabled={!canEdit}
                onChange={(n) =>
                  patch((current) => ({
                    ...current,
                    login: { ...current.login, strike_reset_seconds: n },
                  }))
                }
              />
            </div>
            <Field label="封禁阶梯（秒，逗号分隔）" hint="每次达到失败阈值后按顺序使用，最后一个会重复">
              <input
                className="field font-mono text-[11px]"
                value={ladderText}
                onChange={(event) => setLadderText(event.target.value)}
                disabled={!canEdit}
              />
            </Field>
            <ToggleRow
              label="对回环地址也启用登录限流"
              hint="默认关闭，避免本机调试被误封"
              checked={value.login.throttle_loopback}
              disabled={!canEdit}
              onChange={(checked) =>
                patch((current) => ({
                  ...current,
                  login: { ...current.login, throttle_loopback: checked },
                }))
              }
            />
          </Section>
        </Card>

        <Card>
          <Section title="会话与 Cookie" hint="修改空闲/绝对有效期后对新登录生效；已有服务重启后统一">
            <div className="grid grid-cols-2 gap-3">
              <NumberInput
                label="空闲超时（秒）"
                value={value.session.idle_seconds}
                disabled={!canEdit}
                onChange={(n) =>
                  patch((current) => ({
                    ...current,
                    session: { ...current.session, idle_seconds: n },
                  }))
                }
              />
              <NumberInput
                label="绝对有效期（秒）"
                value={value.session.absolute_seconds}
                disabled={!canEdit}
                onChange={(n) =>
                  patch((current) => ({
                    ...current,
                    session: { ...current.session, absolute_seconds: n },
                  }))
                }
              />
            </div>
            <Field label="Cookie Secure 策略">
              <select
                className="field [&>option]:bg-surface [&>option]:text-ink"
                value={value.session.secure_cookie}
                disabled={!canEdit}
                onChange={(event) =>
                  patch((current) => ({
                    ...current,
                    session: { ...current.session, secure_cookie: event.target.value },
                  }))
                }
              >
                <option value="auto">自动（HTTPS 时启用）</option>
                <option value="always">始终启用</option>
                <option value="never">从不启用</option>
              </select>
            </Field>
            <ToggleRow
              label="信任 X-Forwarded-Proto"
              hint="放在反向代理后时用于判断 HTTPS 与 Secure Cookie"
              checked={value.session.trust_forwarded_proto}
              disabled={!canEdit}
              onChange={(checked) =>
                patch((current) => ({
                  ...current,
                  session: { ...current.session, trust_forwarded_proto: checked },
                }))
              }
            />
          </Section>
        </Card>
      </div>
    </div>
  )
}

function Section({
  title,
  hint,
  children,
}: {
  title: string
  hint: string
  children: ReactNode
}) {
  return (
    <div className="space-y-3">
      <div className="flex items-center gap-2 text-[13px] font-medium text-ink">
        <ShieldCheck size={15} className="text-accent" />
        {title}
      </div>
      <p className="text-[10px] leading-relaxed text-muted">{hint}</p>
      {children}
    </div>
  )
}

function ToggleRow({
  label,
  hint,
  checked,
  disabled,
  onChange,
}: {
  label: string
  hint: string
  checked: boolean
  disabled: boolean
  onChange: (checked: boolean) => void
}) {
  return (
    <div className="flex items-center justify-between gap-3 rounded-lg border border-line bg-panel/50 px-3 py-2">
      <div>
        <div className="text-[12px] text-ink">{label}</div>
        <div className="text-[10px] text-muted">{hint}</div>
      </div>
      <Switch checked={checked} onChange={onChange} label={label} disabled={disabled} />
    </div>
  )
}

function NumberInput({
  label,
  value,
  onChange,
  disabled,
}: {
  label: string
  value: number
  onChange: (value: number) => void
  disabled: boolean
}) {
  return (
    <Field label={label}>
      <input
        className="field h-9"
        type="number"
        min={1}
        value={value}
        disabled={disabled}
        onChange={(event) => onChange(Math.max(1, Number(event.target.value) || 1))}
      />
    </Field>
  )
}
