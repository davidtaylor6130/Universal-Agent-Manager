import { createContext, forwardRef, useContext, useLayoutEffect, useRef, useState } from 'react'
import type { ButtonHTMLAttributes, KeyboardEvent, ReactNode } from 'react'
import { cx } from './cx'

export type TabsVariant = 'underline' | 'pill'

interface TabsContextValue {
  value: string
  onSelect: (value: string) => void
}
const TabsContext = createContext<TabsContextValue | null>(null)

export interface TabsProps<T extends string> {
  value: T
  onChange: (value: T) => void
  'aria-label': string
  variant?: TabsVariant
  className?: string
  children: ReactNode
}

/**
 * Tab list with roving tabindex and arrow/Home/End keys (activation follows
 * focus). One sliding indicator glides under the selected tab; motion stops via
 * the shared reduced-motion and data-motion="off" rules in index.css.
 */
export function Tabs<T extends string>({ value, onChange, 'aria-label': label, variant = 'underline', className, children }: TabsProps<T>) {
  const listRef = useRef<HTMLDivElement>(null)
  const [slot, setSlot] = useState<{ x: number; w: number } | null>(null)
  useLayoutEffect(() => {
    const list = listRef.current
    if (!list) return
    const measure = () => {
      const active = list.querySelector<HTMLElement>('[aria-selected="true"]')
      setSlot(active ? { x: active.offsetLeft, w: active.offsetWidth } : null)
    }
    measure()
    if (typeof ResizeObserver === 'undefined') return
    const observer = new ResizeObserver(measure)
    observer.observe(list)
    return () => observer.disconnect()
  }, [value, children])

  const onKeyDown = (event: KeyboardEvent<HTMLDivElement>) => {
    const tabs = Array.from(listRef.current?.querySelectorAll<HTMLButtonElement>('[role="tab"]:not(:disabled)') ?? [])
    const index = tabs.indexOf(event.target as HTMLButtonElement)
    if (index < 0) return
    const last = tabs.length - 1
    const targets: Record<string, number> = { ArrowRight: index === last ? 0 : index + 1, ArrowLeft: index === 0 ? last : index - 1, Home: 0, End: last }
    const target = targets[event.key]
    if (target === undefined) return
    event.preventDefault()
    tabs[target].focus()
    tabs[target].click()
  }

  return (
    <TabsContext.Provider value={{ value, onSelect: next => onChange(next as T) }}>
      <div role="tablist" aria-label={label} ref={listRef} onKeyDown={onKeyDown} data-variant={variant} className={cx('uam-tabs', className)}>
        {slot && <span aria-hidden className="uam-tabs__indicator" style={{ transform: `translateX(${slot.x}px)`, width: slot.w }} />}
        {children}
      </div>
    </TabsContext.Provider>
  )
}

export interface TabProps extends Omit<ButtonHTMLAttributes<HTMLButtonElement>, 'value'> {
  value: string
  icon?: ReactNode
}

/** One tab inside `Tabs`. Must be rendered within a Tabs. */
export const Tab = forwardRef<HTMLButtonElement, TabProps>(function Tab({ value, icon, className, children, onClick, ...rest }, ref) {
  const context = useContext(TabsContext)
  if (!context) throw new Error('Tab must be rendered inside Tabs')
  const selected = context.value === value
  return (
    <button
      {...rest}
      ref={ref}
      type="button"
      role="tab"
      aria-selected={selected}
      tabIndex={selected ? 0 : -1}
      data-value={value}
      className={cx('uam-tab', className)}
      onClick={(event) => {
        onClick?.(event)
        if (!event.defaultPrevented) context.onSelect(value)
      }}
    >
      {icon}
      {children}
    </button>
  )
})
