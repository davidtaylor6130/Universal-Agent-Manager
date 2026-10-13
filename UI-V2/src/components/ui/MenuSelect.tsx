import { KeyboardEvent, ReactNode, useEffect, useId, useRef, useState } from 'react'
import { Check, ChevronDown } from 'lucide-react'
import { ViewportMenu } from './ViewportMenu'

export interface MenuSelectOption {
  value: string
  label: string
  description?: string
  icon?: ReactNode
  disabled?: boolean
  group?: string
}

export function MenuSelect({
  id,
  label,
  value,
  options,
  onChange,
  disabled = false,
  placeholder,
}: {
  /** Applied to the trigger button so a `<label htmlFor>` can reference it. */
  id?: string
  label: string
  value: string
  options: MenuSelectOption[]
  onChange: (value: string) => void
  disabled?: boolean
  /** Trigger text when no option matches value (for action pickers). */
  placeholder?: string
}) {
  const [open, setOpen] = useState(false)
  const [focusIndex, setFocusIndex] = useState(0)
  const rootRef = useRef<HTMLDivElement>(null)
  const triggerRef = useRef<HTMLButtonElement>(null)
  const listRef = useRef<HTMLDivElement>(null)
  const optionRefs = useRef<Array<HTMLButtonElement | null>>([])
  const listId = useId()
  const matchIndex = options.findIndex((option) => option.value === value)
  const selectedIndex = Math.max(0, matchIndex)
  const selected = matchIndex < 0 && placeholder !== undefined ? undefined : options[selectedIndex]

  useEffect(() => {
    if (!open) return
    setFocusIndex(options[selectedIndex]?.disabled ? options.findIndex((option) => !option.disabled) : selectedIndex)
    const onPointerDown = (event: MouseEvent) => {
      const target = event.target as Node
      if (!rootRef.current?.contains(target) && !listRef.current?.contains(target)) setOpen(false)
    }
    document.addEventListener('mousedown', onPointerDown)
    return () => document.removeEventListener('mousedown', onPointerDown)
  }, [open, selectedIndex])

  useEffect(() => {
    if (!open) return
    if (options[focusIndex]?.disabled) {
      setFocusIndex(options.findIndex((option) => !option.disabled))
      return
    }
    if (options[focusIndex]) optionRefs.current[focusIndex]?.focus()
    else triggerRef.current?.focus()
  }, [focusIndex, open, options])

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
    const enabled = options.flatMap((option, index) => option.disabled ? [] : [index])
    if (enabled.length === 0) return
    if (event.key === 'Home') setFocusIndex(enabled[0])
    else if (event.key === 'End') setFocusIndex(enabled[enabled.length - 1])
    else {
      const current = enabled.indexOf(focusIndex)
      const direction = event.key === 'ArrowDown' ? 1 : -1
      setFocusIndex(current < 0 ? enabled[direction > 0 ? 0 : enabled.length - 1]
        : enabled[(current + direction + enabled.length) % enabled.length])
    }
  }

  return (
    <div ref={rootRef} className="relative">
      <button
        ref={triggerRef}
        id={id}
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
          <span className="truncate" style={selected ? undefined : { color: 'var(--text-2)' }}>{selected?.label ?? placeholder ?? value}</span>
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
          {options.map((option, index) => [
            option.group && option.group !== options[index - 1]?.group && <div key={`group:${option.group}`} role="presentation" className="px-2 pb-1 pt-2 text-[10px] font-semibold uppercase tracking-wider" style={{ color: 'var(--text-3)' }}>{option.group}</div>,
            <button
              key={option.value}
              ref={(element) => { optionRefs.current[index] = element }}
              type="button"
              role="option"
              aria-selected={option.value === value}
              disabled={option.disabled}
              onMouseEnter={() => { if (!option.disabled) setFocusIndex(index) }}
              onClick={() => {
                if (option.disabled) return
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
            </button>,
          ])}
        </ViewportMenu>
      )}
    </div>
  )
}
