interface ChatSearchBarProps {
  value: string
  deepSearch: boolean
  onChange: (value: string) => void
  onClear: () => void
  onToggleDeepSearch: () => void
}

export function ChatSearchBar({ value, deepSearch, onChange, onClear, onToggleDeepSearch }: ChatSearchBarProps) {
  return (
    <div
      className="flex-shrink-0 px-3 py-3"
      style={{ background: 'var(--sidebar-bg)' }}
    >
      <div className="flex items-center gap-2">
        <div
          className="flex min-w-0 flex-1 items-center gap-2 rounded-md px-2"
          style={{
            height: 34,
            background: 'var(--surface)',
            border: '1px solid var(--border)',
            color: 'var(--text-3)',
          }}
        >
          <svg
            width="14"
            height="14"
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            strokeWidth="2"
            strokeLinecap="round"
            strokeLinejoin="round"
            style={{ flexShrink: 0 }}
            aria-hidden="true"
          >
            <circle cx="11" cy="11" r="8" />
            <line x1="21" y1="21" x2="16.65" y2="16.65" />
          </svg>

          <input
            value={value}
            onChange={(event) => onChange(event.target.value)}
            onKeyDown={(event) => {
              if (event.key === 'Escape' && value) {
                onClear()
                event.currentTarget.blur()
              }
            }}
            placeholder="Search chats..."
            aria-label="Search chats"
            className="min-w-0 flex-1 bg-transparent text-xs outline-none"
            style={{
              color: 'var(--text)',
              fontFamily: 'inherit',
            }}
          />

          {value && (
            <button
              type="button"
              title="Clear search"
              aria-label="Clear search"
              className="flex items-center justify-center rounded transition-colors duration-100"
              style={{
                width: 18,
                height: 18,
                background: 'transparent',
                border: 'none',
                color: 'var(--text-3)',
                cursor: 'pointer',
                flexShrink: 0,
              }}
              onMouseEnter={(event) => {
                event.currentTarget.style.background = 'var(--sidebar-item-hover)'
                event.currentTarget.style.color = 'var(--text-2)'
              }}
              onMouseLeave={(event) => {
                event.currentTarget.style.background = 'transparent'
                event.currentTarget.style.color = 'var(--text-3)'
              }}
              onClick={onClear}
            >
              <svg
                width="12"
                height="12"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                strokeWidth="2.4"
                strokeLinecap="round"
                aria-hidden="true"
              >
                <line x1="18" y1="6" x2="6" y2="18" />
                <line x1="6" y1="6" x2="18" y2="18" />
              </svg>
            </button>
          )}
        </div>
        <button
          type="button"
          title={deepSearch ? 'Search chat contents' : 'Search chat names'}
          aria-label={deepSearch ? 'Search chat contents' : 'Search chat names'}
          className="flex items-center justify-center rounded-md"
          style={{
            width: 34,
            height: 34,
            border: '1px solid var(--border)',
            background: deepSearch ? 'var(--accent)' : 'var(--surface)',
            color: deepSearch ? 'white' : 'var(--text-2)',
          }}
          onClick={onToggleDeepSearch}
        >
          <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
            <path d="M2.5 4h11M4.5 8h7M6.5 12h3" />
            <path d="M11.5 3v2M6.5 7v2M9.5 11v2" />
          </svg>
        </button>
      </div>
    </div>
  )
}
