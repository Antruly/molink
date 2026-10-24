import { useEffect, useState } from 'react'
import { ArrowUpRight, Info, Server, ShieldCheck } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { startUpdate, checkForUpdate, cachedUpdate, type UpdateInfo } from '../../lib/update'
import { useAppStore } from '../../store/app'

export function NoticePanel() {
  const { t } = useTranslation()
  const localMode = useAppStore((state) => state.localMode)
  const server = useAppStore((state) => state.server)
  const device = useAppStore((state) => state.device)
  const version = server?.version ?? device?.version ?? '0.9.0'
  const [update, setUpdate] = useState<UpdateInfo | null>(() =>
    cachedUpdate(version),
  )

  useEffect(() => {
    if (!localMode) return
    let alive = true
    if (!update) {
      void checkForUpdate(version).then((info) => {
        if (alive && info) setUpdate(info)
      })
    }
    return () => {
      alive = false
    }
  }, [localMode, version, update])

  const info = server?.server
  const enabled = info?.enabled ?? true
  const online = info?.online ?? false
  const registered = info?.registered ?? false

  const statusText = !enabled
    ? t('simple.noticeServerOff')
    : registered && online
      ? t('simple.noticeServerOnline')
      : registered
        ? t('simple.noticeServerConnecting')
        : t('simple.noticeServerUnknown')

  return (
    <section className="card flex h-full flex-col gap-2 p-3">
      <div className="flex items-center gap-1.5 text-[12px] font-medium text-ink">
        <ShieldCheck size={14} className="text-accent" />
        {t('simple.notices')}
      </div>

      <div className="rounded-xl border border-line-soft bg-panel/40 p-2.5">
        <div className="flex items-center gap-1.5 text-[11px] text-ink">
          <Server size={12} className={online ? 'text-accent' : 'text-muted'} />
          {statusText}
        </div>
        <div className="mt-1 truncate font-mono text-[10px] text-muted">
          {enabled ? info?.url || t('simple.serverPending') : t('simple.lanOnly')}
        </div>
        {info?.last_error && enabled ? (
          <div className="mt-1 line-clamp-2 text-[10px] leading-relaxed text-warn">
            {info.last_error}
          </div>
        ) : null}
      </div>

      {update ? (
        <div className="rounded-xl border border-accent/40 bg-accent-soft p-2.5">
          <div className="text-[11px] font-medium text-accent">
            {t('simple.updateAvailable', { version: update.version })}
          </div>
          <button
            type="button"
            className="mt-1.5 flex items-center gap-1 rounded-lg bg-accent px-2 py-1 text-[10px] font-medium text-on-accent"
            onClick={() => startUpdate(update)}
          >
            <ArrowUpRight size={11} />
            {t('simple.updateNow')}
          </button>
        </div>
      ) : null}

      <div className="rounded-xl border border-line-soft p-2.5 text-[10px] leading-relaxed text-muted">
        <div className="flex gap-1.5">
          <Info size={12} className="mt-0.5 shrink-0 text-accent" />
          {t('simple.natHint')}
        </div>
      </div>

      {server?.open_sessions ? (
        <div className="mt-auto text-[10px] text-muted">
          {t('simple.activeSessions', { count: server.open_sessions })}
        </div>
      ) : null}
    </section>
  )
}
