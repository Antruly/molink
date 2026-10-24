export type Role = 'admin' | 'operator' | 'viewer'
export type Tone = 'ok' | 'danger'
export type Notify = (message: string, tone?: Tone) => void

export function roleRank(role: string): number {
  if (role === 'admin') return 3
  if (role === 'operator') return 2
  if (role === 'viewer') return 1
  return 0
}
