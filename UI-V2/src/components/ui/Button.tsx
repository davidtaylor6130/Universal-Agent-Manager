import { forwardRef } from 'react'
import type { ButtonHTMLAttributes, ReactNode } from 'react'
import { Loader2 } from 'lucide-react'
import { cx } from './cx'

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
}

/**
 * Primary button primitive. Variants + sizes + full state set (hover, active,
 * disabled, loading, focus-visible ring). All redesigned buttons compose this
 * rather than restyling <button> ad-hoc.
 */
export const Button = forwardRef<HTMLButtonElement, ButtonProps>(function Button(
  { variant = 'secondary', size = 'md', leadingIcon, trailingIcon, loading = false, block = false, disabled, className, children, ...rest },
  ref
) {
  return (
    <button
      ref={ref}
      className={cx('uam-btn', `uam-btn--${variant}`, `uam-btn--${size}`, block && 'uam-btn--block', className)}
      disabled={disabled || loading}
      aria-busy={loading || undefined}
      {...rest}
    >
      {loading ? <Loader2 className="uam-btn__spinner" size={size === 'sm' ? 13 : 15} aria-hidden /> : leadingIcon}
      {children != null && <span className="uam-btn__label">{children}</span>}
      {!loading && trailingIcon}
    </button>
  )
})
