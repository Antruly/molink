import { useState, type FormEvent } from 'react'
import { LogIn, Server } from 'lucide-react'
import { ApiError, api, type AuthResult } from '../lib/api'
import { Field } from '../components/ui'

export function LoginPage({ onAuthed }: { onAuthed: (result: AuthResult) => void }) {
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [error, setError] = useState<string | null>(null)
  const [retry, setRetry] = useState(0)
  const [busy, setBusy] = useState(false)

  const submit = async (event: FormEvent) => {
    event.preventDefault()
    setError(null)
    setRetry(0)
    setBusy(true)
    try {
      const result = await api.login(username.trim(), password)
      onAuthed(result)
    } catch (caught) {
      if (caught instanceof ApiError) {
        setError(caught.message)
        setRetry(caught.retryAfter)
      } else {
        setError('登录失败')
      }
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="grid min-h-screen place-items-center p-4">
      <div className="w-full max-w-sm">
        <div className="mb-6 text-center">
          <span className="mx-auto grid h-14 w-14 place-items-center rounded-2xl bg-accent-soft text-accent">
            <Server size={26} />
          </span>
          <h1 className="mt-3 text-xl font-semibold text-ink">moLink Server</h1>
          <p className="mt-1 text-[12px] text-muted">登录管理控制台</p>
        </div>
        <form onSubmit={submit} className="card space-y-3">
          <Field label="用户名">
            <input
              className="field"
              value={username}
              onChange={(event) => setUsername(event.target.value)}
              autoComplete="username"
              autoFocus
              required
            />
          </Field>
          <Field label="密码">
            <input
              className="field"
              type="password"
              value={password}
              onChange={(event) => setPassword(event.target.value)}
              autoComplete="current-password"
              required
            />
          </Field>
          {error ? (
            <div className="rounded-lg border border-danger/40 bg-danger/10 px-3 py-2 text-[11px] text-danger">
              {error}
              {retry > 0 ? `（请 ${retry} 秒后重试）` : ''}
            </div>
          ) : null}
          <button type="submit" className="btn btn-primary h-10 w-full gap-2" disabled={busy}>
            <LogIn size={16} />
            {busy ? '正在登录…' : '登录'}
          </button>
        </form>
      </div>
    </div>
  )
}
