import { forwardRef } from 'react'
import type { ButtonHTMLAttributes, ReactNode } from 'react'
import { cx } from './cx'
import { Tooltip } from './Tooltip'

export type IconButtonVariant = 'ghost' | 'solid' | 'danger'
export type IconButtonSize = 'sm' | 'md' | 'lg'

export interface IconButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  /** Lucide icon element. */
  icon: ReactNode
  /** Required accessible label — also the default tooltip. Enforces "tooltips on everything". */
  label: string
  /** Optional richer tooltip text shown instead of `label` (label stays the stable accessible name). */
  tooltip?: string
  variant?: IconButtonVariant
  size?: IconButtonSize
  /** Keyboard shortcut hint shown in the tooltip. */
  shortcut?: string
  tooltipSide?: 'top' | 'right' | 'bottom' | 'left'
  /** Active/pressed visual state (e.g. a toggled panel). */
  active?: boolean
}

/**
 * Icon-only button. The `label` prop is mandatory and powers both the
 * accessible name and the tooltip, so no icon-only control ships unlabeled.
 */
export const IconButton = forwardRef<HTMLButtonElement, IconButtonProps>(function IconButton(
  { icon, label, tooltip, variant = 'ghost', size = 'md', shortcut, tooltipSide = 'top', active = false, className, ...rest },
  ref
) {
  return (
    <Tooltip label={tooltip ?? label} shortcut={shortcut} side={tooltipSide}>
      <button
        ref={ref}
        type="button"
        aria-label={label}
        aria-pressed={active || undefined}
        className={cx('uam-iconbtn', `uam-iconbtn--${variant}`, `uam-iconbtn--${size}`, active && 'is-active', className)}
        {...rest}
      >
        {icon}
      </button>
    </Tooltip>
  )
})
