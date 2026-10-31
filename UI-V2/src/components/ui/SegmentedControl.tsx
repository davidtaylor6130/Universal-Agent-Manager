import { useLayoutEffect, useRef, useState } from 'react'
import type { ReactNode } from 'react'
import { Button } from './Button'
import { cx } from './cx'

export interface SegmentedOption<T extends string | number> {
  value: T
  label: string
  icon?: ReactNode
}

export interface SegmentedControlProps<T extends string | number> {
  options: SegmentedOption<T>[]
  value: T
  onChange: (value: T) => void
  'aria-label': string
  size?: 'sm' | 'md'
  disabled?: boolean
  className?: string
}

/**
 * Single-choice toggle group. One sliding indicator glides behind the pressed
 * option (transform + width only); motion stops via the shared reduced-motion
 * and data-motion="off" rules in index.css.
 */
export function SegmentedControl<T extends string | number>({ options, value, onChange, 'aria-label': label, size = 'sm', disabled = false, className }: SegmentedControlProps<T>) {
  const groupRef = useRef<HTMLDivElement>(null)
  const [slot, setSlot] = useState<{ x: number; w: number } | null>(null)
  useLayoutEffect(() => {
    const group = groupRef.current
    if (!group) return
    const measure = () => {
      const active = group.querySelector<HTMLElement>('[aria-pressed="true"]')
      setSlot(active ? { x: active.offsetLeft, w: active.offsetWidth } : null)
    }
    measure()
    if (typeof ResizeObserver === 'undefined') return
    const observer = new ResizeObserver(measure)
    observer.observe(group)
    return () => observer.disconnect()
  }, [value, options.length])
  return <div role="group" aria-label={label} ref={groupRef} className={cx('uam-seg', className)}>
    {slot && <span aria-hidden className="uam-seg__indicator" style={{ transform: `translateX(${slot.x}px)`, width: slot.w }} />}
    {options.map(option => <Button key={String(option.value)} size={size} variant="ghost" disabled={disabled} leadingIcon={option.icon} aria-pressed={value === option.value} onClick={() => onChange(option.value)}>{option.label}</Button>)}
  </div>
}
