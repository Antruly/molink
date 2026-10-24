import { useState, type FormEvent } from 'react'
import { Server, ShieldCheck } from 'lucide-react'
import { ApiError, api, type AuthResult } from '../lib/api'
import { Field } from '../components/ui'

export function SetupPage({ onAuthed }: { onAuthed: (result: AuthResult) => void }) {
  const [username, setUsername] = useState('admin')
  const [displayName, setDisplayName] = useState('Administrator')
  const [password, setPassword] = useState('')
  const [confirm, setConfirm] = useState('')
  const [setupToken, setSetupToken] = useState('')
  const [error, setError] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)

  const submit = async (event: FormEvent) => {
    event.preventDefault()
    setError(null)
    if (password.length < 10) {
      setError('密码至少需要 10 个字符')
      return
    }
    if (password !== confirm) {
      setError('两次输入的密码不一致')
      return
    }
    setBusy(true)
    try {
      const result = await api.setup({
        username: username.trim(),
        display_name: displayName.trim(),
        password,
        setup_token: setupToken.trim() || undefined,
      })
      onAuthed(result)
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : '初始化失败')
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="grid min-h-screen place-items-center p-4">
      <div className="w-full max-w-md">
        <div className="mb-6 text-center">
          <span className="mx-auto grid h-14 w-14 place-items-center rounded-2xl bg-accent-soft text-accent">
            <Server size={26} />
          </span>
          <h1 className="mt-3 text-xl font-semibold text-ink">初始化 moLink Server</h1>
          <p className="mt-1 text-[12px] text-muted">
            创建第一个管理员账户。设备目录与安全策略会保存在服务端数据库中。
          </p>
        </div>
        <form onSubmit={submit} className="card space-y-3">
          <Field label="管理员用户名">
            <input
              className="field"
              value={username}
              onChange={(event) => setUsername(event.target.value)}
              autoComplete="username"
              required
            />
          </Field>
          <Field label="显示名称">
            <input
              className="field"
              value={displayName}
              onChange={(event) => setDisplayName(event.target.value)}
            />
          </Field>
          <Field label="密码" hint="至少 10 个字符">
            <input
              className="field"
              type="password"
              value={password}
              onChange={(event) => setPassword(event.target.value)}
              autoComplete="new-password"
              required
            />
          </Field>
          <Field label="确认密码">
            <input
              className="field"
              type="password"
              value={confirm}
              onChange={(event) => setConfirm(event.target.value)}
              autoComplete="new-password"
              required
            />
          </Field>
          <Field
            label="初始化令牌（可选）"
            hint="仅当管理端绑定非回环地址且尚未初始化时需要，令牌会打印在服务端日志中"
          >
            <input
              className="field"
              value={setupToken}
              onChange={(event) => setSetupToken(event.target.value)}
              placeholder="通常留空"
            />
          </Field>
          {error ? (
            <div className="rounded-lg border border-danger/40 bg-danger/10 px-3 py-2 text-[11px] text-danger">
              {error}
            </div>
          ) : null}
          <button type="submit" className="btn btn-primary h-10 w-full gap-2" disabled={busy}>
            <ShieldCheck size={16} />
            {busy ? '正在创建…' : '创建管理员并进入控制台'}
          </button>
        </form>
      </div>
    </div>
  )
}
