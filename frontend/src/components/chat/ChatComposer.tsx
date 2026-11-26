import { Image as ImageIcon, Paperclip, Send, SquareArrowUp } from 'lucide-react'
import { useEffect, useRef, useState, type ClipboardEvent } from 'react'
import { useTranslation } from 'react-i18next'
import { filesFromClipboard } from './chat-hooks'

/// The text box plus the attachment entry points. Validation and upload live
/// in the panel; this component only decides what the user asked for.
export function ChatComposer({
  onSendText,
  onSendFiles,
  attachEnabled = true,
  disabled = false,
  placeholder,
  hint,
}: {
  onSendText: (text: string) => boolean | Promise<boolean>
  onSendFiles: (files: File[]) => void
  attachEnabled?: boolean
  disabled?: boolean
  placeholder: string
  hint?: string | null
}) {
  const { t } = useTranslation()
  const [text, setText] = useState('')
  const [menu, setMenu] = useState(false)
  const imageInput = useRef<HTMLInputElement>(null)
  const fileInput = useRef<HTMLInputElement>(null)
  const textarea = useRef<HTMLTextAreaElement>(null)

  useEffect(() => {
    const element = textarea.current
    if (!element) return
    element.style.height = 'auto'
    element.style.height = `${Math.min(120, element.scrollHeight)}px`
  }, [text])

  const send = async () => {
    const value = text.trim()
    if (value === '' || disabled) return
    const ok = await onSendText(value)
    if (ok) setText('')
  }

  const pick = (target: 'image' | 'file') => {
    setMenu(false)
    if (target === 'image') imageInput.current?.click()
    else fileInput.current?.click()
  }

  const take = (list: FileList | null) => {
    if (!list) return
    const files = Array.from(list)
    if (files.length > 0) onSendFiles(files)
  }

  const onPaste = (event: ClipboardEvent<HTMLTextAreaElement>) => {
    const files = filesFromClipboard(event.clipboardData)
    if (files.length === 0 || !attachEnabled) return
    event.preventDefault()
    onSendFiles(files)
  }

  return (
    <div className="shrink-0 border-t border-line-soft">
      {hint ? (
        <div className="px-3 pt-2 text-[10px] leading-snug text-warn">
          {hint}
        </div>
      ) : null}
      <div className="flex items-end gap-1.5 p-2.5">
        <div className="relative">
          <button
            type="button"
            onClick={() => setMenu((open) => !open)}
            disabled={disabled || !attachEnabled}
            className="grid h-9 w-9 shrink-0 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink disabled:opacity-40"
            title={t('chat.attachments')}
            aria-label={t('chat.attachments')}
            aria-expanded={menu}
          >
            <Paperclip size={16} />
          </button>
          {menu ? (
            <>
              <div
                className="fixed inset-0 z-30"
                onPointerDown={() => setMenu(false)}
              />
              <div className="absolute bottom-10 left-0 z-40 w-40 overflow-hidden rounded-lg border border-line bg-surface py-1 text-[12px] shadow-2xl">
                <button
                  type="button"
                  onClick={() => pick('image')}
                  className="flex w-full items-center gap-2 px-3 py-2 text-left text-ink transition hover:bg-panel"
                >
                  <ImageIcon size={14} className="text-muted" />
                  {t('chat.pickImage')}
                </button>
                <button
                  type="button"
                  onClick={() => pick('file')}
                  className="flex w-full items-center gap-2 px-3 py-2 text-left text-ink transition hover:bg-panel"
                >
                  <SquareArrowUp size={14} className="text-muted" />
                  {t('chat.pickFile')}
                </button>
              </div>
            </>
          ) : null}
          <input
            ref={imageInput}
            type="file"
            accept="image/*,video/*"
            multiple
            hidden
            onChange={(event) => {
              take(event.target.files)
              event.target.value = ''
            }}
          />
          <input
            ref={fileInput}
            type="file"
            multiple
            hidden
            onChange={(event) => {
              take(event.target.files)
              event.target.value = ''
            }}
          />
        </div>

        <textarea
          ref={textarea}
          value={text}
          onChange={(event) => setText(event.target.value)}
          onPaste={onPaste}
          onKeyDown={(event) => {
            if (event.key === 'Enter' && !event.shiftKey &&
                !event.nativeEvent.isComposing) {
              event.preventDefault()
              void send()
            }
          }}
          rows={2}
          maxLength={4096}
          disabled={disabled}
          placeholder={placeholder}
          className="field min-h-9 max-h-[120px] resize-none !py-2 text-[12px]"
        />
        <button
          type="button"
          onClick={() => void send()}
          disabled={disabled || text.trim() === ''}
          className="btn btn-primary h-9 w-9 shrink-0 !px-0"
          aria-label={t('chat.send')}
          title={t('chat.send')}
        >
          <Send size={14} />
        </button>
      </div>
    </div>
  )
}
