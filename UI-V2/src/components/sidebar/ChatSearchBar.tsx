import { useEffect, useRef, useState } from 'react'
import type { ChatSearchFilters, ChatStatusFilterId } from './chatSearch'

interface ChatSearchProviderFilterOption {
  id: string
  label: string
}

interface ChatSearchBarProps {
  value: string
  deepSearch: boolean
  filters: ChatSearchFilters
  providerOptions: ChatSearchProviderFilterOption[]
  onChange: (value: string) => void
  onClear: () => void
  onToggleDeepSearch: () => void
  onToggleProviderFilter: (providerId: string) => void
  onToggleStatusFilter: (statusId: ChatStatusFilterId) => void
  onClearFilters: () => void
}

const STATUS_OPTIONS: Array<{ id: ChatStatusFilterId; label: string }> = [
  { id: 'pinned', label: 'Pinned' },
  { id: 'running', label: 'Running' },
  { id: 'attention', label: 'Needs attention' },
  { id: 'done', label: 'Done' },
  { id: 'idle', label: 'Idle' },
]

export function ChatSearchBar({
  value,
  deepSearch,
  filters,
  providerOptions,
  onChange,
  onClear,
  onToggleDeepSearch,
  onToggleProviderFilter,
  onToggleStatusFilter,
  onClearFilters,
}: ChatSearchBarProps) {
  const [filtersOpen, setFiltersOpen] = useState(false)
  const popoverRef = useRef<HTMLDivElement>(null)
  const activeFilterCount = filters.providerIds.length + filters.statusIds.length + (deepSearch ? 1 : 0)

  useEffect(() => {
    if (!filtersOpen) return

    const onMouseDown = (event: MouseEvent) => {
      const target = event.target
      if (target instanceof Node && popoverRef.current && !popoverRef.current.contains(target)) {
        setFiltersOpen(false)
      }
    }

    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        setFiltersOpen(false)
      }
    }

    document.addEventListener('mousedown', onMouseDown)
    document.addEventListener('keydown', onKeyDown)
    return () => {
      document.removeEventListener('mousedown', onMouseDown)
      document.removeEventListener('keydown', onKeyDown)
    }
  }, [filtersOpen])

  return (
    <div
      className="flex h-12 flex-shrink-0 items-center px-4"
      style={{ background: 'var(--surface)' }}
    >
      <div className="flex w-full items-center gap-2">
        <div
          className="flex min-w-0 flex-1 items-center gap-2 rounded-md px-2"
          style={{
            height: 32,
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
        <div className="relative" ref={popoverRef}>
          <button
            type="button"
            title={activeFilterCount > 0 ? `${activeFilterCount} chat filter${activeFilterCount === 1 ? '' : 's'} active` : 'Filter chats'}
            aria-label="Filter chats"
            aria-expanded={filtersOpen}
            className="relative flex items-center justify-center rounded-md"
            style={{
              width: 32,
              height: 32,
              border: '1px solid var(--border)',
              background: activeFilterCount > 0 ? 'var(--accent)' : 'var(--surface)',
              color: activeFilterCount > 0 ? 'white' : 'var(--text-2)',
            }}
            onClick={() => setFiltersOpen((open) => !open)}
          >
            <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
              <path d="M2.5 4h11M4.5 8h7M6.5 12h3" />
              <path d="M11.5 3v2M6.5 7v2M9.5 11v2" />
            </svg>
            {activeFilterCount > 0 && (
              <span
                aria-hidden="true"
                className="absolute -right-1 -top-1 flex items-center justify-center rounded-full text-[9px] font-semibold"
                style={{
                  minWidth: 14,
                  height: 14,
                  background: 'var(--surface)',
                  color: 'var(--accent)',
                  border: '1px solid var(--border)',
                }}
              >
                {activeFilterCount}
              </span>
            )}
          </button>

          {filtersOpen && (
            <div
              className="absolute right-0 z-40 mt-2 w-56 rounded-md p-2 text-xs shadow-xl"
              style={{
                background: 'var(--surface-up)',
                border: '1px solid var(--border-bright)',
                color: 'var(--text)',
              }}
            >
              <div className="mb-2 flex items-center justify-between gap-2">
                <span className="font-semibold" style={{ color: 'var(--text-2)' }}>Filters</span>
                {(activeFilterCount > 0) && (
                  <button
                    type="button"
                    className="rounded px-1.5 py-0.5 text-[10px]"
                    style={{ border: '1px solid var(--border)', color: 'var(--text-3)' }}
                    onClick={() => {
                      if (deepSearch) onToggleDeepSearch()
                      onClearFilters()
                    }}
                  >
                    Clear
                  </button>
                )}
              </div>

              <button
                type="button"
                className="mb-2 flex w-full items-center justify-between rounded px-2 py-1.5"
                style={{
                  background: deepSearch ? 'color-mix(in srgb, var(--accent) 16%, var(--surface))' : 'transparent',
                  color: deepSearch ? 'var(--text)' : 'var(--text-2)',
                  border: '1px solid var(--border)',
                }}
                onClick={onToggleDeepSearch}
              >
                <span>Search contents</span>
                <span style={{ color: deepSearch ? 'var(--accent)' : 'var(--text-3)' }}>{deepSearch ? 'On' : 'Off'}</span>
              </button>

              <div className="mb-1 text-[10px] uppercase" style={{ color: 'var(--text-3)', letterSpacing: '0.08em' }}>
                State
              </div>
              <div className="mb-2 space-y-1">
                {STATUS_OPTIONS.map((option) => {
                  const active = filters.statusIds.includes(option.id)
                  return (
                    <button
                      key={option.id}
                      type="button"
                      className="flex w-full items-center justify-between rounded px-2 py-1"
                      style={{
                        background: active ? 'color-mix(in srgb, var(--accent) 14%, var(--surface))' : 'transparent',
                        color: active ? 'var(--text)' : 'var(--text-2)',
                      }}
                      onClick={() => onToggleStatusFilter(option.id)}
                    >
                      <span>{option.label}</span>
                      <span aria-hidden="true">{active ? 'On' : ''}</span>
                    </button>
                  )
                })}
              </div>

              <div className="mb-1 text-[10px] uppercase" style={{ color: 'var(--text-3)', letterSpacing: '0.08em' }}>
                Provider
              </div>
              <div className="max-h-40 space-y-1 overflow-auto">
                {providerOptions.map((provider) => {
                  const active = filters.providerIds.includes(provider.id)
                  return (
                    <button
                      key={provider.id}
                      type="button"
                      className="flex w-full items-center justify-between rounded px-2 py-1"
                      style={{
                        background: active ? 'color-mix(in srgb, var(--accent) 14%, var(--surface))' : 'transparent',
                        color: active ? 'var(--text)' : 'var(--text-2)',
                      }}
                      onClick={() => onToggleProviderFilter(provider.id)}
                    >
                      <span className="truncate">{provider.label}</span>
                      <span aria-hidden="true">{active ? 'On' : ''}</span>
                    </button>
                  )
                })}
              </div>
            </div>
          )}
        </div>
      </div>
    </div>
  )
}
