import { useRef, useState } from 'react'
import { Upload, X } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { AVATAR_PRESETS, fileToAvatar, initialOf, platformEmoji, presetColor } from '../../lib/avatar'
import { Modal } from '../ui'

export function Avatar({
  value,
  name,
  platform,
  size = 44,
  showPlatform = true,
}: {
  value?: string
  name: string
  platform?: string
  size?: number
  showPlatform?: boolean
}) {
  const preset = presetColor(value)
  const color = preset?.color ?? 'var(--accent-strong)'
  const ring = preset?.ring ?? 'rgba(94,234,212,.25)'
  const isImage = value?.startsWith('data:image/')
  return (
    <span
      className="relative inline-grid shrink-0 place-items-center overflow-visible rounded-2xl"
      style={{ width: size, height: size }}
    >
      <span
        className="grid h-full w-full place-items-center overflow-hidden rounded-2xl border text-[15px] font-semibold"
        style={{
          borderColor: ring,
          background: isImage
            ? 'var(--panel)'
            : `linear-gradient(145deg, ${color}33, ${color}14)`,
          color,
          boxShadow: `0 0 0 1px ${ring}`,
        }}
      >
        {isImage ? (
          <img
            src={value}
            alt=""
            className="h-full w-full object-cover"
            draggable={false}
          />
        ) : (
          initialOf(name)
        )}
      </span>
      {showPlatform ? (
        <span
          className="absolute -bottom-1 -right-1 grid h-5 w-5 place-items-center rounded-full border border-line bg-surface text-[10px] shadow-sm"
          title={platform || 'device'}
        >
          {platformEmoji(platform)}
        </span>
      ) : null}
    </span>
  )
}

export function AvatarPicker({
  open,
  value,
  name,
  busy = false,
  onClose,
  onSelect,
}: {
  open: boolean
  value?: string
  name: string
  busy?: boolean
  onClose: () => void
  onSelect: (value: string) => Promise<void> | void
}) {
  const { t } = useTranslation()
  const inputRef = useRef<HTMLInputElement>(null)
  const [error, setError] = useState<string | null>(null)
  const [working, setWorking] = useState(false)

  const choose = async (next: string) => {
    setError(null)
    setWorking(true)
    try {
      await onSelect(next)
      onClose()
    } catch (caught) {
      setError(
        caught instanceof Error && caught.message === 'image_too_large'
          ? t('simple.avatarTooLarge')
          : t('simple.avatarFailed'),
      )
    } finally {
      setWorking(false)
    }
  }

  const upload = async (file: File | undefined) => {
    if (!file) return
    setError(null)
    setWorking(true)
    try {
      const data = await fileToAvatar(file)
      await choose(data)
    } catch (caught) {
      setError(
        caught instanceof Error && caught.message === 'image_too_large'
          ? t('simple.avatarTooLarge')
          : t('simple.avatarFailed'),
      )
    } finally {
      setWorking(false)
    }
  }

  return (
    <Modal
      open={open}
      title={t('simple.chooseAvatar')}
      onClose={onClose}
      footer={
        <button type="button" className="btn h-8" onClick={onClose}>
          {t('common.cancel')}
        </button>
      }
    >
      <div className="flex items-center gap-3">
        <Avatar value={value} name={name} size={56} showPlatform={false} />
        <div className="text-[11px] leading-relaxed text-muted">
          {t('simple.avatarHint')}
        </div>
      </div>
      <div className="mt-4 grid grid-cols-6 gap-2">
        {AVATAR_PRESETS.map((preset) => (
          <button
            key={preset.key}
            type="button"
            className="grid aspect-square place-items-center rounded-xl border text-[13px] font-semibold transition hover:scale-105"
            style={{
              borderColor: value === preset.key ? preset.color : 'var(--line)',
              background: `linear-gradient(145deg, ${preset.color}33, ${preset.color}14)`,
              color: preset.color,
            }}
            onClick={() => void choose(preset.key)}
            disabled={busy || working}
            title={preset.key}
          >
            {initialOf(name)}
          </button>
        ))}
      </div>
      <div className="mt-4 flex flex-wrap gap-2">
        <input
          ref={inputRef}
          type="file"
          accept="image/*"
          className="hidden"
          onChange={(event) => {
            const file = event.target.files?.[0]
            event.target.value = ''
            void upload(file)
          }}
        />
        <button
          type="button"
          className="btn h-9 gap-1.5"
          disabled={busy || working}
          onClick={() => inputRef.current?.click()}
        >
          <Upload size={14} />
          {t('simple.uploadAvatar')}
        </button>
        <button
          type="button"
          className="btn btn-quiet h-9 gap-1.5"
          disabled={busy || working}
          onClick={() => void choose('')}
        >
          <X size={14} />
          {t('simple.removeAvatar')}
        </button>
        {working ? <span className="self-center text-[11px] text-muted">{t('simple.saving')}</span> : null}
      </div>
      {error ? <div className="mt-3 text-[11px] text-danger">{error}</div> : null}
    </Modal>
  )
}
