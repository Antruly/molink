import { Check, ShieldCheck, X } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { useAppStore } from '../store/app'

/// Shown only to the local user. A remote peer without a code asks for
/// permission, and every request must be approved explicitly.
export function AccessRequestDialog() {
  const { t } = useTranslation()
  const server = useAppStore((state) => state.server)
  const decide = useAppStore((state) => state.decideAccessRequest)
  const requests = server?.local ? (server.access_requests ?? []) : []
  if (requests.length === 0) return null

  return (
    <div
      data-local-input="true"
      className="fixed inset-x-0 top-4 z-50 mx-auto w-[min(92vw,28rem)] space-y-2"
    >
      {requests.map((request) => (
        <div
          key={request.id}
          className="card border border-accent/40 p-4 shadow-2xl"
        >
          <div className="flex items-start gap-3">
            <ShieldCheck size={18} className="mt-0.5 shrink-0 text-accent" />
            <div className="min-w-0 flex-1">
              <div className="text-[13px] font-medium text-ink">
                {t('access.requestTitle')}
              </div>
              <div className="mt-1 truncate text-[12px] text-muted">
                {request.name || request.client || request.peer_ip}
              </div>
              <div className="font-mono text-[11px] text-muted/80">
                {request.peer_ip}
              </div>
              <div className="mt-3 flex gap-2">
                <button
                  type="button"
                  className="btn btn-primary h-8 gap-1 px-2 text-[11px]"
                  onClick={() => void decide(request.id, true)}
                >
                  <Check size={13} />
                  {t('access.allow')}
                </button>
                <button
                  type="button"
                  className="btn h-8 gap-1 px-2 text-[11px]"
                  onClick={() => void decide(request.id, false)}
                >
                  <X size={13} />
                  {t('access.deny')}
                </button>
              </div>
            </div>
          </div>
        </div>
      ))}
    </div>
  )
}
