import { useState, useRef, useCallback } from 'react'

interface InputBarProps {
  onSend: (content: string) => void
  disabled?: boolean
  placeholder?: string
}

export function InputBar({ onSend, disabled = false, placeholder = 'Message…' }: InputBarProps) {
  const [value, setValue] = useState('')
  const [isDragging, setIsDragging] = useState(false)
  const textareaRef = useRef<HTMLTextAreaElement>(null)
  const fileInputRef = useRef<HTMLInputElement>(null)

  const adjustHeight = () => {
    const el = textareaRef.current
    if (!el) return
    el.style.height = 'auto'
    el.style.height = `${Math.min(el.scrollHeight, 180)}px`
  }

  const handleSend = useCallback(() => {
    const trimmed = value.trim()
    if (!trimmed || disabled) return
    onSend(trimmed)
    setValue('')
    if (textareaRef.current) textareaRef.current.style.height = 'auto'
  }, [value, disabled, onSend])

  const handleKeyDown = (e: React.KeyboardEvent<HTMLTextAreaElement>) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault()
      handleSend()
    }
  }

  const handleDrop = (e: React.DragEvent) => {
    e.preventDefault()
    setIsDragging(false)
    // In Stage 1, just show the file names as text
    const files = Array.from(e.dataTransfer.files)
    if (files.length > 0) {
      const names = files.map((f) => f.name).join(', ')
      setValue((prev) => prev + (prev ? '\n' : '') + `[Files: ${names}]`)
    }
  }

  const canSend = value.trim().length > 0 && !disabled

  return (
    <div
      className="flex-shrink-0 transition-colors duration-150"
      onDragOver={(e) => { e.preventDefault(); setIsDragging(true) }}
      onDragLeave={() => setIsDragging(false)}
      onDrop={handleDrop}
      style={{
        background: 'var(--surface)',
        borderTop: isDragging ? '1px solid var(--accent)' : '1px solid var(--border)',
      }}
    >
      <div className="px-4 py-3">
        {/* Textarea */}
        <div
          className="flex items-end gap-2 rounded-xl px-3 py-2.5 transition-all duration-150"
          style={{
            background: 'var(--surface-up)',
            border: isDragging ? '1px solid var(--accent)' : '1px solid var(--border)',
          }}
          onFocus={() => {}}
        >
          <textarea
            ref={textareaRef}
            value={value}
            onChange={(e) => { setValue(e.target.value); adjustHeight() }}
            onKeyDown={handleKeyDown}
            disabled={disabled}
            placeholder={placeholder}
            rows={1}
            className="flex-1 bg-transparent outline-none resize-none text-sm"
            style={{
              color: 'var(--text)',
              fontFamily: 'inherit',
              lineHeight: '1.55',
              minHeight: 22,
              maxHeight: 180,
              caretColor: 'var(--accent)',
            }}
          />

          {/* Attach button */}
          <button
            type="button"
            onClick={() => fileInputRef.current?.click()}
            title="Attach file"
            className="flex-shrink-0 rounded-lg flex items-center justify-center transition-colors duration-150"
            style={{
              width: 26,
              height: 26,
              background: 'transparent',
              color: 'var(--text-3)',
              border: 'none',
              cursor: 'pointer',
              marginBottom: 1,
            }}
            onMouseEnter={(e) => { e.currentTarget.style.color = 'var(--text-2)' }}
            onMouseLeave={(e) => { e.currentTarget.style.color = 'var(--text-3)' }}
          >
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
              <path d="M21.44 11.05l-9.19 9.19a6 6 0 0 1-8.49-8.49l9.19-9.19a4 4 0 0 1 5.66 5.66l-9.2 9.19a2 2 0 0 1-2.83-2.83l8.49-8.48" />
            </svg>
          </button>

          {/* Send button */}
          <button
            type="button"
            onClick={handleSend}
            disabled={!canSend}
            title="Send (Enter)"
            className="flex-shrink-0 rounded-lg flex items-center justify-center transition-all duration-150"
            style={{
              width: 26,
              height: 26,
              background: canSend ? 'var(--accent)' : 'var(--surface-high)',
              color: canSend ? '#fff' : 'var(--text-3)',
              border: 'none',
              cursor: canSend ? 'pointer' : 'default',
              marginBottom: 1,
              opacity: canSend ? 1 : 0.5,
            }}
          >
            <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round">
              <line x1="22" y1="2" x2="11" y2="13" />
              <polygon points="22 2 15 22 11 13 2 9 22 2" />
            </svg>
          </button>
        </div>

        {/* Hint */}
        <div className="flex items-center justify-between mt-1.5 px-1">
          <span className="text-xs" style={{ color: 'var(--text-3)', fontSize: 10 }}>
            Enter to send · Shift+Enter for newline · Drop files to attach
          </span>
        </div>
      </div>

      {/* Hidden file input */}
      <input
        ref={fileInputRef}
        type="file"
        multiple
        className="hidden"
        onChange={(e) => {
          const files = Array.from(e.target.files ?? [])
          if (files.length > 0) {
            const names = files.map((f) => f.name).join(', ')
            setValue((prev) => prev + (prev ? '\n' : '') + `[Files: ${names}]`)
          }
          e.target.value = ''
        }}
      />
    </div>
  )
}
