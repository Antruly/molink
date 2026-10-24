import { useCallback, useEffect, useState, type FormEvent } from 'react'
import { KeyRound, Pencil, Plus, ShieldCheck, Trash2, UserRound } from 'lucide-react'
import { ApiError, api, type User } from '../lib/api'
import { roleRank, type Notify, type Role } from '../lib/types'
import { formatRelative } from '../lib/format'
import {
  Badge,
  Empty,
  ErrorBanner,
  Field,
  Modal,
  PageHeader,
  Spinner,
  Switch,
} from '../components/ui'

const ROLES: Array<{ value: string; label: string }> = [
  { value: 'admin', label: '管理员（全部权限）' },
  { value: 'operator', label: '操作员（设备/封禁/审计）' },
  { value: 'viewer', label: '只读' },
]

export function UsersPage({ role, notify }: { role: Role; notify: Notify }) {
  const [users, setUsers] = useState<User[]>([])
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)

  const [createOpen, setCreateOpen] = useState(false)
  const [createName, setCreateName] = useState('')
  const [createDisplay, setCreateDisplay] = useState('')
  const [createRole, setCreateRole] = useState('operator')
  const [createPassword, setCreatePassword] = useState('')
  const [createBusy, setCreateBusy] = useState(false)
  const [createError, setCreateError] = useState<string | null>(null)

  const [editing, setEditing] = useState<User | null>(null)
  const [editDisplay, setEditDisplay] = useState('')
  const [editRole, setEditRole] = useState('viewer')
  const [editDisabled, setEditDisabled] = useState(false)
  const [editBusy, setEditBusy] = useState(false)
  const [editError, setEditError] = useState<string | null>(null)

  const [resetting, setResetting] = useState<User | null>(null)
  const [resetPassword, setResetPassword] = useState('')
  const [resetBusy, setResetBusy] = useState(false)
  const [resetError, setResetError] = useState<string | null>(null)

  const load = useCallback(async () => {
    setLoading(true)
    setError(null)
    try {
      const result = await api.users()
      setUsers(result.items)
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : '无法加载管理员')
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => {
    void load()
  }, [load])

  if (roleRank(role) < 3) {
    return <ErrorBanner message="只有管理员可以管理账户。" />
  }

  const openCreate = () => {
    setCreateName('')
    setCreateDisplay('')
    setCreateRole('operator')
    setCreatePassword('')
    setCreateError(null)
    setCreateOpen(true)
  }

  const submitCreate = async (event: FormEvent) => {
    event.preventDefault()
    setCreateBusy(true)
    setCreateError(null)
    try {
      await api.createUser({
        username: createName.trim(),
        display_name: createDisplay.trim(),
        role: createRole,
        password: createPassword,
      })
      notify('管理员已创建')
      setCreateOpen(false)
      await load()
    } catch (caught) {
      setCreateError(caught instanceof ApiError ? caught.message : '创建失败')
    } finally {
      setCreateBusy(false)
    }
  }

  const openEdit = (user: User) => {
    setEditing(user)
    setEditDisplay(user.display_name)
    setEditRole(user.role)
    setEditDisabled(user.disabled)
    setEditError(null)
  }

  const submitEdit = async (event: FormEvent) => {
    event.preventDefault()
    if (!editing) return
    setEditBusy(true)
    setEditError(null)
    try {
      await api.updateUser(editing.id, {
        display_name: editDisplay.trim(),
        role: editRole,
        disabled: editDisabled,
      })
      notify('账户已更新')
      setEditing(null)
      await load()
    } catch (caught) {
      setEditError(caught instanceof ApiError ? caught.message : '保存失败')
    } finally {
      setEditBusy(false)
    }
  }

  const remove = async (user: User) => {
    if (!window.confirm(`确定删除账户「${user.username}」吗？`)) return
    try {
      await api.deleteUser(user.id)
      notify('账户已删除')
      await load()
    } catch (caught) {
      notify(caught instanceof ApiError ? caught.message : '删除失败', 'danger')
    }
  }

  const openReset = (user: User) => {
    setResetting(user)
    setResetPassword('')
    setResetError(null)
  }

  const submitReset = async (event: FormEvent) => {
    event.preventDefault()
    if (!resetting) return
    setResetBusy(true)
    setResetError(null)
    try {
      await api.resetPassword(resetting.id, resetPassword)
      notify('密码已重置，该账户的其他会话已退出')
      setResetting(null)
    } catch (caught) {
      setResetError(caught instanceof ApiError ? caught.message : '重置失败')
    } finally {
      setResetBusy(false)
    }
  }

  return (
    <div>
      <PageHeader
        title="管理员"
        description="管理可以登录控制台的账户与角色。"
        actions={
          <button type="button" className="btn btn-primary h-8 gap-1 px-2 text-[11px]" onClick={openCreate}>
            <Plus size={13} />
            新建账户
          </button>
        }
      />
      <ErrorBanner message={error} />
      {loading && users.length === 0 ? (
        <Spinner />
      ) : users.length === 0 ? (
        <Empty>还没有管理员账户。</Empty>
      ) : (
        <div className="card overflow-hidden p-0">
          <div className="overflow-x-auto">
            <table className="data-table">
              <thead>
                <tr>
                  <th>账户</th>
                  <th>角色</th>
                  <th>状态</th>
                  <th>最近登录</th>
                  <th>创建时间</th>
                  <th className="text-right">操作</th>
                </tr>
              </thead>
              <tbody>
                {users.map((user) => (
                  <tr key={user.id}>
                    <td>
                      <div className="flex items-center gap-2">
                        <span className="grid h-7 w-7 place-items-center rounded-lg bg-panel text-accent">
                          {user.role === 'admin' ? <ShieldCheck size={14} /> : <UserRound size={14} />}
                        </span>
                        <div>
                          <div className="font-medium text-ink">{user.display_name || user.username}</div>
                          <div className="font-mono text-[10px] text-muted">@{user.username}</div>
                        </div>
                      </div>
                    </td>
                    <td>
                      <Badge tone={user.role === 'admin' ? 'accent' : 'muted'}>{user.role}</Badge>
                    </td>
                    <td>
                      <Badge tone={user.disabled ? 'danger' : 'ok'}>
                        {user.disabled ? '已禁用' : '正常'}
                      </Badge>
                    </td>
                    <td className="text-muted">
                      {user.last_login_ms ? formatRelative(user.last_login_ms) : '从未'}
                    </td>
                    <td className="text-muted">{formatRelative(user.created_ms)}</td>
                    <td className="text-right">
                      <div className="flex justify-end gap-1">
                        <button
                          type="button"
                          className="btn btn-quiet h-7 w-7 !px-0"
                          title="编辑"
                          onClick={() => openEdit(user)}
                        >
                          <Pencil size={12} />
                        </button>
                        <button
                          type="button"
                          className="btn btn-quiet h-7 w-7 !px-0"
                          title="重置密码"
                          onClick={() => openReset(user)}
                        >
                          <KeyRound size={12} />
                        </button>
                        <button
                          type="button"
                          className="btn btn-quiet h-7 w-7 !px-0 hover:!border-danger/40 hover:!text-danger"
                          title="删除"
                          onClick={() => void remove(user)}
                        >
                          <Trash2 size={12} />
                        </button>
                      </div>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      )}

      <Modal
        open={createOpen}
        title="新建管理员"
        onClose={() => setCreateOpen(false)}
        footer={
          <>
            <button type="button" className="btn h-8" onClick={() => setCreateOpen(false)}>
              取消
            </button>
            <button
              type="submit"
              form="create-user-form"
              className="btn btn-primary h-8"
              disabled={createBusy}
            >
              {createBusy ? '创建中…' : '创建'}
            </button>
          </>
        }
      >
        <form id="create-user-form" className="space-y-3" onSubmit={submitCreate}>
          <Field label="用户名">
            <input
              className="field"
              value={createName}
              onChange={(event) => setCreateName(event.target.value)}
              required
            />
          </Field>
          <Field label="显示名称">
            <input
              className="field"
              value={createDisplay}
              onChange={(event) => setCreateDisplay(event.target.value)}
            />
          </Field>
          <Field label="角色">
            <select
              className="field [&>option]:bg-surface [&>option]:text-ink"
              value={createRole}
              onChange={(event) => setCreateRole(event.target.value)}
            >
              {ROLES.map((item) => (
                <option key={item.value} value={item.value}>
                  {item.label}
                </option>
              ))}
            </select>
          </Field>
          <Field label="初始密码" hint="至少 10 个字符">
            <input
              className="field"
              type="password"
              value={createPassword}
              onChange={(event) => setCreatePassword(event.target.value)}
              required
            />
          </Field>
          <ErrorBanner message={createError} />
        </form>
      </Modal>

      <Modal
        open={editing !== null}
        title={`编辑账户 ${editing?.username ?? ''}`}
        onClose={() => setEditing(null)}
        footer={
          <>
            <button type="button" className="btn h-8" onClick={() => setEditing(null)}>
              取消
            </button>
            <button
              type="submit"
              form="edit-user-form"
              className="btn btn-primary h-8"
              disabled={editBusy}
            >
              {editBusy ? '保存中…' : '保存'}
            </button>
          </>
        }
      >
        <form id="edit-user-form" className="space-y-3" onSubmit={submitEdit}>
          <Field label="显示名称">
            <input
              className="field"
              value={editDisplay}
              onChange={(event) => setEditDisplay(event.target.value)}
            />
          </Field>
          <Field label="角色">
            <select
              className="field [&>option]:bg-surface [&>option]:text-ink"
              value={editRole}
              onChange={(event) => setEditRole(event.target.value)}
            >
              {ROLES.map((item) => (
                <option key={item.value} value={item.value}>
                  {item.label}
                </option>
              ))}
            </select>
          </Field>
          <div className="flex items-center justify-between rounded-lg border border-line bg-panel/60 px-3 py-2">
            <div>
              <div className="text-[12px] text-ink">禁用账户</div>
              <div className="text-[10px] text-muted">禁用后该账户的所有会话立即失效</div>
            </div>
            <Switch checked={editDisabled} onChange={setEditDisabled} label="禁用账户" />
          </div>
          <ErrorBanner message={editError} />
        </form>
      </Modal>

      <Modal
        open={resetting !== null}
        title={`重置密码 ${resetting?.username ?? ''}`}
        onClose={() => setResetting(null)}
        footer={
          <>
            <button type="button" className="btn h-8" onClick={() => setResetting(null)}>
              取消
            </button>
            <button
              type="submit"
              form="reset-password-form"
              className="btn btn-primary h-8"
              disabled={resetBusy}
            >
              {resetBusy ? '重置中…' : '重置密码'}
            </button>
          </>
        }
      >
        <form id="reset-password-form" className="space-y-3" onSubmit={submitReset}>
          <Field label="新密码" hint="至少 10 个字符；重置后该账户的其他会话会退出">
            <input
              className="field"
              type="password"
              value={resetPassword}
              onChange={(event) => setResetPassword(event.target.value)}
              required
            />
          </Field>
          <ErrorBanner message={resetError} />
        </form>
      </Modal>
    </div>
  )
}
