import { Check, Trash2 } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Modal } from './ui'

/// Edits the note for one device name. The note is shared by every card that
/// shows that name (recent, saved, LAN, online), so the dialog always speaks
/// about the name rather than one particular address.
export function DeviceNoteDialog({
  open,
  name,
  value,
  onClose,
  onSave,
}: {
  open: boolean
  name: string
  value: string
  onClose: () => void
  onSave: (note: string) => void
}) {
  // Remounting per open gives the draft a fresh start without an effect that
  // resets state after paint.
  if (!open) return null
  return (
    <DeviceNoteForm
      name={name}
      value={value}
      onClose={onClose}
      onSave={onSave}
    />
  )
}

function DeviceNoteForm({
  name,
  value,
  onClose,
  onSave,
}: {
  name: string
  value: string
  onClose: () => void
  onSave: (note: string) => void
}) {
  const { t } = useTranslation()
  const [draft, setDraft] = useState(value)

  const save = () => {
    onSave(draft)
    onClose()
  }

  return (
    <Modal
      open
      title={t('deviceNote.title')}
      onClose={onClose}
      footer={
        <>
          {draft ? (
            <button
              type="button"
              className="btn mr-auto"
              onClick={() => {
                setDraft('')
                onSave('')
                onClose()
              }}
            >
              <Trash2 size={13} />
              {t('deviceNote.clear')}
            </button>
          ) : null}
          <button type="button" className="btn" onClick={onClose}>
            {t('deviceNote.cancel')}
          </button>
          <button type="button" className="btn btn-primary" onClick={save}>
            <Check size={13} />
            {t('deviceNote.save')}
          </button>
        </>
      }
    >
      <div className="min-w-0">
        <div className="text-[10px] uppercase tracking-[0.14em] text-muted">
          {t('deviceNote.device')}
        </div>
        <div className="mt-0.5 truncate text-[12px] font-medium text-ink">
          {name || t('deviceNote.unnamed')}
        </div>
      </div>
      <input
        value={draft}
        autoFocus
        maxLength={80}
        placeholder={t('deviceNote.placeholder')}
        onChange={(event) => setDraft(event.target.value)}
        onKeyDown={(event) => {
          if (event.key === 'Enter') {
            event.preventDefault()
            save()
          }
        }}
        className="field mt-3"
      />
      <p className="mt-2 text-[10px] leading-relaxed text-muted">
        {t('deviceNote.hint')}
      </p>
    </Modal>
  )
}
