import { KeyboardEvent, ReactNode, useEffect, useId, useRef, useState } from 'react'
import { Check, ChevronDown } from 'lucide-react'
import { ViewportMenu } from './ViewportMenu'

export interface MenuSelectOption {
  value: string
  label: string
  description?: string
  icon?: ReactNode
}

export function MenuSelect({
  label,
  value,
  options,
  onChange,
  disabled = false,
}: {
  label: string
  value: string
  options: MenuSelectOption[]
  onChange: (value: string) => void
  disabled?: boolean
}) {
  const [open, setOpen] = useState(false)
  const [focusIndex, setFocusIndex] = useState(0)
  const rootRef = useRef<HTMLDivElement>(null)
  const triggerRef = useRef<HTMLButtonElement>(null)
  const listRef = useRef<HTMLDivElement>(null)
  const optionRefs = useRef<Array<HTMLButtonElement | null>>([])
  const listId = useId()
  const selectedIndex = Math.max(0, options.findIndex((option) => option.value === value))
  const selected = options[selectedIndex]

  useEffect(() => {
    if (!open) return
    setFocusIndex(selectedIndex)
    const onPointerDown = (event: MouseEvent) => {
      const target = event.target as Node
      if (!rootRef.current?.contains(target) && !listRef.current?.contains(target)) setOpen(false)
    }
    document.addEventListener('mousedown', onPointerDown)
    return () => document.removeEventListener('mousedown', onPointerDown)
  }, [open, selectedIndex])

  useEffect(() => {
    if (open) optionRefs.current[focusIndex]?.focus()
  }, [focusIndex, open])

  const close = () => {
    setOpen(false)
    triggerRef.current?.focus()
  }

  const onListKeyDown = (event: KeyboardEvent<HTMLDivElement>) => {
    if (event.key === 'Escape') {
      event.preventDefault()
      close()
      return
    }
    if (event.key === 'Tab') {
      close()
      return
    }
    if (!['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) return
    event.preventDefault()
    if (event.key === 'Home') setFocusIndex(0)
    else if (event.key === 'End') setFocusIndex(options.length - 1)
    else setFocusIndex((index) => (index + (event.key === 'ArrowDown' ? 1 : -1) + options.length) % options.length)
  }

  return (
    <div ref={rootRef} className="relative">
      <button
        ref={triggerRef}
        type="button"
        role="combobox"
        title={label}
        aria-label={label}
        aria-controls={listId}
        aria-expanded={open}
        aria-haspopup="listbox"
        disabled={disabled}
        onClick={() => setOpen((current) => !current)}
        onKeyDown={(event) => {
          if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
            event.preventDefault()
            setOpen(true)
          }
        }}
        className="uam-menu-select__trigger flex w-full items-center justify-between gap-2 rounded-md px-3 py-2 text-left text-sm"
      >
        <span className="flex min-w-0 items-center gap-2 truncate">
          {selected?.icon && <span data-menu-select-icon aria-hidden className="shrink-0">{selected.icon}</span>}
          <span className="truncate">{selected?.label ?? value}</span>
        </span>
        <ChevronDown className={open ? 'uam-menu-select__chevron is-open' : 'uam-menu-select__chevron'} size={14} aria-hidden />
      </button>
      {open && (
        <ViewportMenu
          ref={listRef}
          anchorRef={triggerRef}
          id={listId}
          role="listbox"
          aria-label={label}
          onKeyDown={onListKeyDown}
          className="rounded-md p-1"
          style={{ minWidth: triggerRef.current?.getBoundingClientRect().width, maxHeight: 240, background: 'var(--surface)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-3)' }}
        >
          {options.map((option, index) => (
            <button
              key={option.value}
              ref={(element) => { optionRefs.current[index] = element }}
              type="button"
              role="option"
              aria-selected={option.value === value}
              onMouseEnter={() => setFocusIndex(index)}
              onClick={() => {
                onChange(option.value)
                close()
              }}
              className={`uam-menu-select__option flex w-full items-start gap-2 rounded px-2 py-2 text-left text-sm${option.value === value ? ' is-selected' : ''}`}
            >
              {option.icon && <span data-menu-select-icon aria-hidden className="mt-0.5 shrink-0">{option.icon}</span>}
              <span className="min-w-0 flex-1">
                <span className="block">{option.label}</span>
                {option.description && <span className="block text-[11px]" style={{ color: 'var(--text-3)' }}>{option.description}</span>}
              </span>
              <Check size={13} aria-hidden className="mt-0.5 shrink-0" style={{ opacity: option.value === value ? 1 : 0 }} />
            </button>
          ))}
        </ViewportMenu>
      )}
    </div>
  )
}
