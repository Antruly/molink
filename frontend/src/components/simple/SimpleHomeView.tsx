import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { ApiError, lookupDevice } from '../../lib/api'
import { parseTarget } from '../../lib/target'
import { useAppStore, type SessionIntent } from '../../store/app'
import { ConnectCard } from './ConnectCard'
import { DeviceList, type DeviceItem } from './DeviceList'
import { NoticePanel } from './NoticePanel'
import { SelfCard } from './SelfCard'
import { StatusBar } from './StatusBar'

export function SimpleHomeView() {
  const { t } = useTranslation()
  const localMode = useAppStore((state) => state.localMode)
  const server = useAppStore((state) => state.server)
  const createSession = useAppStore((state) => state.createSession)
  const preferredMode = useAppStore((state) => state.preferredMode)

  const [mode, setMode] = useState<SessionIntent>('desktop')
  const [input, setInput] = useState('')
  const [code, setCode] = useState('')
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [notice, setNotice] = useState<string | null>(null)

  const flash = (message: string) => {
    setNotice(message)
    window.setTimeout(() => setNotice(null), 2600)
  }

  const connect = (item: DeviceItem, intent: SessionIntent, tokenValue?: string) => {
    const token = (tokenValue ?? code).trim()
    const requestAccess = token === ''
    createSession(
      item.host,
      item.port,
      token,
      item.label,
      false,
      requestAccess,
      preferredMode,
      intent,
      item.id,
      item.platform,
      item.avatar,
    )
    if (requestAccess) flash(t('simple.requestSent'))
    setError(null)
  }

  const connectInput = async () => {
    setError(null)
    const target = parseTarget(input)
    if (!target) {
      setError(t('simple.badAddress'))
      return
    }
    setBusy(true)
    try {
      if (target.kind === 'id') {
        const info = server?.server
        if (!info?.enabled || !info.url) {
          setError(t('simple.serverRequired'))
          return
        }
        const device = await lookupDevice(info.url, target.id ?? '')
        if (!device.online) {
          setError(t('simple.deviceOffline'))
          return
        }
        const host = device.address || device.peer_ip
        connect(
          {
            key: `id:${device.id}`,
            id: device.id,
            host,
            port: device.port,
            label: device.name || device.display_id,
            platform: device.platform,
            avatar: device.avatar,
            source: 'recent',
            lastSeen: Date.now(),
          },
          mode,
          code,
        )
      } else {
        connect(
          {
            key: `host:${target.host}:${target.port}`,
            host: target.host,
            port: target.port,
            label: target.label,
            source: 'recent',
            lastSeen: Date.now(),
          },
          mode,
          code,
        )
      }
    } catch (caught) {
      if (caught instanceof ApiError) {
        if (caught.code === 'not_found' || caught.code === 'unknown_device') {
          setError(t('simple.deviceNotFound'))
        } else if (caught.code === 'ip_denied' || caught.code === 'ip_banned') {
          setError(t('simple.directoryDenied'))
        } else if (caught.code === 'rate_limited') {
          setError(t('simple.directoryBusy'))
        } else {
          setError(t('simple.lookupFailed'))
        }
      } else {
        setError(t('simple.lookupFailed'))
      }
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="mx-auto w-full max-w-7xl px-4 py-5 sm:px-6">
      <div className="grid gap-4 lg:grid-cols-12">
        {localMode ? (
          <div className="lg:col-span-4">
            <SelfCard />
          </div>
        ) : null}
        <div className={localMode ? 'lg:col-span-8' : 'lg:col-span-12'}>
          <ConnectCard
            mode={mode}
            setMode={setMode}
            input={input}
            setInput={setInput}
            code={code}
            setCode={setCode}
            busy={busy}
            error={error}
            onSubmit={() => void connectInput()}
          />
        </div>
      </div>

      <div className="mt-4 grid gap-4 lg:grid-cols-12">
        <div className="lg:col-span-3">
          <NoticePanel />
        </div>
        <div className="lg:col-span-9">
          <DeviceList
            mode={mode}
            onConnect={(item, intent) => {
              const token = code.trim() || item.token || ''
              connect(item, intent, token)
            }}
          />
        </div>
      </div>

      <StatusBar />

      {notice ? (
        <div className="fixed bottom-14 left-1/2 z-50 -translate-x-1/2 rounded-full border border-accent/40 bg-surface/95 px-4 py-2 text-[11px] text-accent shadow-2xl backdrop-blur">
          {notice}
        </div>
      ) : null}
    </div>
  )
}
