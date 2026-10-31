import { forwardRef, useId } from 'react'
import type { ButtonHTMLAttributes, MouseEvent, ReactNode } from 'react'
import { Loader2 } from 'lucide-react'
import { cx } from './cx'
import { Tooltip } from './Tooltip'

export type ButtonVariant = 'primary' | 'secondary' | 'ghost' | 'danger'
export type ButtonSize = 'sm' | 'md' | 'lg'

export interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: ButtonVariant
  size?: ButtonSize
  /** Lucide icon element rendered before the label. */
  leadingIcon?: ReactNode
  /** Lucide icon element rendered after the label. */
  trailingIcon?: ReactNode
  /** Shows a spinner and disables interaction. */
  loading?: boolean
  /** Stretch to fill the container width. */
  block?: boolean
  /** Opt-in: when disabled, stay focusable (aria-disabled) and explain why in a Tooltip. */
  disabledReason?: string
}

/** Blocks clicks without removing focus; shared by Button and IconButton. */
export const blockDisabledClick = (event: MouseEvent) => event.preventDefault()

/**
 * Primary button primitive. Variants + sizes + full state set (hover, active,
 * disabled, loading, focus-visible ring). All redesigned buttons compose this
 * rather than restyling <button> ad-hoc.
 */
export const Button = forwardRef<HTMLButtonElement, ButtonProps>(function Button(
  { variant = 'secondary', size = 'md', leadingIcon, trailingIcon, loading = false, block = false, disabled, disabledReason, className, children, onClick, ...rest },
  ref
) {
  const blocked = Boolean(disabled || loading)
  const reasonId = useId()
  const reasonActive = Boolean(disabledReason && blocked)
  return (
    <Tooltip label={reasonActive ? disabledReason : undefined}>
      <button
        ref={ref}
        type="button"
        className={cx('uam-btn', `uam-btn--${variant}`, `uam-btn--${size}`, block && 'uam-btn--block', className)}
        disabled={reasonActive ? undefined : blocked}
        aria-disabled={reasonActive || undefined}
        aria-describedby={reasonActive ? reasonId : undefined}
        aria-busy={loading || undefined}
        onClick={reasonActive ? blockDisabledClick : onClick}
        {...rest}
      >
        {loading ? <Loader2 className="uam-btn__spinner" size={size === 'sm' ? 13 : 15} aria-hidden /> : leadingIcon}
        {children != null && <span className="uam-btn__label">{children}</span>}
        {!loading && trailingIcon}
        {reasonActive && <span id={reasonId} hidden>{disabledReason}</span>}
      </button>
    </Tooltip>
  )
})
