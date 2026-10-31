import { forwardRef, useId } from 'react'
import type { ButtonHTMLAttributes, ReactNode } from 'react'
import { cx } from './cx'
import { Tooltip } from './Tooltip'
import { blockDisabledClick } from './Button'

export type IconButtonVariant = 'ghost' | 'solid' | 'danger'
export type IconButtonSize = 'sm' | 'md' | 'lg'

export interface IconButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  /** Lucide icon element. */
  icon: ReactNode
  /** Required accessible label — also the default tooltip. Enforces "tooltips on everything". */
  label: string
  /** Optional richer tooltip text shown instead of `label` (label stays the stable accessible name). `false` skips this button's own Tooltip when the caller already wraps it. */
  tooltip?: string | false
  variant?: IconButtonVariant
  size?: IconButtonSize
  /** Keyboard shortcut hint shown in the tooltip. */
  shortcut?: string
  tooltipSide?: 'top' | 'right' | 'bottom' | 'left'
  /** Active/pressed visual state (e.g. a toggled panel). */
  active?: boolean
  /** Opt-in: when disabled, stay focusable (aria-disabled) and explain why in the tooltip instead of `label`. */
  disabledReason?: string
}

/**
 * Icon-only button. The `label` prop is mandatory and powers both the
 * accessible name and the tooltip, so no icon-only control ships unlabeled.
 */
export const IconButton = forwardRef<HTMLButtonElement, IconButtonProps>(function IconButton(
  { icon, label, tooltip, variant = 'ghost', size = 'md', shortcut, tooltipSide = 'top', active = false, disabledReason, disabled, className, onClick, ...rest },
  ref
) {
  const reasonId = useId()
  const reasonActive = Boolean(disabledReason && disabled)
  const tipOff = tooltip === false
  return (
    <Tooltip label={tipOff ? undefined : reasonActive ? disabledReason : (tooltip ?? label)} shortcut={tipOff || reasonActive ? undefined : shortcut} side={tooltipSide}>
      <button
        ref={ref}
        type="button"
        aria-label={label}
        aria-pressed={active || undefined}
        disabled={reasonActive ? undefined : disabled}
        aria-disabled={reasonActive || undefined}
        aria-describedby={reasonActive ? reasonId : undefined}
        onClick={reasonActive ? blockDisabledClick : onClick}
        className={cx('uam-iconbtn', `uam-iconbtn--${variant}`, `uam-iconbtn--${size}`, active && 'is-active', className)}
        {...rest}
      >
        {icon}
        {reasonActive && <span id={reasonId} hidden>{disabledReason}</span>}
      </button>
    </Tooltip>
  )
})
