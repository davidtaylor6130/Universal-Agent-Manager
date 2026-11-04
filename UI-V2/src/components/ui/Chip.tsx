import { forwardRef } from 'react'
import type { ButtonHTMLAttributes, ReactNode } from 'react'
import { cx } from './cx'

export type ChipTone = 'neutral' | 'danger'
export type ChipSize = 'sm' | 'md'

export interface ChipProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  /** Defined = toggle chip: exposes aria-pressed and the selected look. */
  selected?: boolean
  tone?: ChipTone
  icon?: ReactNode
  size?: ChipSize
}

/**
 * Compact choice button (`.uam-choice-button` look) for option sets, pagers and
 * inline actions. Native disabled, all button attributes pass through.
 */
export const Chip = forwardRef<HTMLButtonElement, ChipProps>(function Chip(
  { selected, tone = 'neutral', icon, size = 'md', className, children, type = 'button', ...rest },
  ref
) {
  return (
    <button
      ref={ref}
      type={type}
      aria-pressed={selected}
      data-selected={selected}
      data-tone={tone}
      className={cx('uam-choice-button', 'uam-chip', `uam-chip--${size}`, className)}
      {...rest}
    >
      {icon}
      {children}
    </button>
  )
})
