import { MessageSquare, Phone, Send, X } from 'lucide-react'
import { useEffect, useRef, useState, type FormEvent } from 'react'
import { useTranslation } from 'react-i18next'
import type { ChatMessage } from '../lib/client'

function timeLabel(ts: number): string {
  if (!ts) return ''
  try {
    return new Date(ts).toLocaleTimeString(undefined, {
      hour: '2-digit',
      minute: '2-digit',
    })
  } catch {
    return ''
  }
}

/// Chat drawer shared by the local UI and a controlling client. The core keeps
/// one log per machine, so a message typed on either side appears on both.
export function ChatPanel({
  open,
  onClose,
  messages,
  onSend,
  remoteLabel,
  callAvailable = false,
  callPhase = 'idle',
  onCall,
}: {
  open: boolean
  onClose: () => void
  messages: ChatMessage[]
  onSend: (text: string) => void
  remoteLabel: string
  callAvailable?: boolean
  callPhase?: 'idle' | 'ringing' | 'active'
  onCall?: () => void
}) {
  const { t } = useTranslation()
  const [text, setText] = useState('')
  const listRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!open) return
    const element = listRef.current
    if (element) element.scrollTop = element.scrollHeight
  }, [messages, open])

  if (!open) return null

  const send = () => {
    const value = text.trim()
    if (value === '') return
    onSend(value)
    setText('')
  }

  const submit = (event: FormEvent) => {
    event.preventDefault()
    send()
  }

  return (
    <div
      className="absolute inset-y-0 right-0 z-40 flex w-[min(92vw,340px)] flex-col border-l border-line bg-surface/95 shadow-2xl backdrop-blur-md"
      onPointerDown={(event) => event.stopPropagation()}
      onPointerUp={(event) => event.stopPropagation()}
    >
      <div className="flex h-11 shrink-0 items-center gap-2 border-b border-line-soft px-3">
        <MessageSquare size={15} className="text-accent" />
        <span className="text-[13px] font-medium">{t('chat.title')}</span>
        <span className="chip ml-1 !px-1.5 !py-0 text-[10px]">
          {remoteLabel}
        </span>
        <div className="ml-auto flex items-center gap-1">
          {callAvailable ? (
            <button
              type="button"
              onClick={onCall}
              disabled={callPhase !== 'idle'}
              className="flex items-center gap-1 rounded-md border border-line px-1.5 py-1 text-[10px] text-muted transition hover:border-accent/40 hover:text-ink disabled:opacity-50"
              title={callPhase === 'idle' ? t('call.start') : t('call.inCall')}
              aria-label={t('call.start')}
            >
              <Phone size={12} />
              {callPhase === 'idle' ? t('call.start') : t('call.inCall')}
            </button>
          ) : null}
          <button
            type="button"
            onClick={onClose}
            className="grid h-7 w-7 place-items-center rounded-md text-muted transition hover:bg-panel hover:text-ink"
            aria-label={t('chat.close')}
            title={t('chat.close')}
          >
            <X size={15} />
          </button>
        </div>
      </div>

      <div ref={listRef} className="min-h-0 flex-1 space-y-2 overflow-y-auto px-3 py-3">
        {messages.length === 0 ? (
          <div className="rounded-xl border border-dashed border-line px-3 py-8 text-center text-[12px] leading-relaxed text-muted">
            {t('chat.empty')}
          </div>
        ) : (
          messages.map((message) => {
            const mine = message.from !== 'host'
            return (
              <div
                key={`${message.id}-${message.ts}`}
                className={mine ? 'flex justify-end' : 'flex justify-start'}
              >
                <div
                  className={
                    'max-w-[85%] rounded-2xl px-3 py-2 text-[12px] leading-relaxed shadow-sm ' +
                    (mine
                      ? 'rounded-br-md bg-accent text-on-accent'
                      : 'rounded-bl-md border border-line bg-panel text-ink')
                  }
                >
                  <div className="whitespace-pre-wrap break-words">
                    {message.text}
                  </div>
                  <div
                    className={
                      'mt-1 text-right text-[9px] ' +
                      (mine ? 'text-on-accent/60' : 'text-muted/70')
                    }
                  >
                    {mine ? t('chat.remote') : t('chat.host')} ·{' '}
                    {timeLabel(message.ts)}
                  </div>
                </div>
              </div>
            )
          })
        )}
      </div>

      <form
        onSubmit={submit}
        className="flex shrink-0 items-end gap-2 border-t border-line-soft p-2.5"
      >
        <textarea
          value={text}
          onChange={(event) => setText(event.target.value)}
          onKeyDown={(event) => {
            if (event.key === 'Enter' && !event.shiftKey) {
              event.preventDefault()
              send()
            }
          }}
          rows={2}
          maxLength={4096}
          placeholder={t('chat.placeholder')}
          className="field min-h-9 resize-none !py-2 text-[12px]"
        />
        <button
          type="submit"
          disabled={text.trim() === ''}
          className="btn btn-primary h-9 w-9 shrink-0 !px-0"
          aria-label={t('chat.send')}
          title={t('chat.send')}
        >
          <Send size={14} />
        </button>
      </form>
    </div>
  )
}
