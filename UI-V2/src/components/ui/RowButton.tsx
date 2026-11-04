import { forwardRef } from 'react'
import type { ButtonHTMLAttributes } from 'react'
import { cx } from './cx'

export interface RowButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  /** Marks the current row (aria-current); visuals stay with the passed className. */
  selected?: boolean
}

/**
 * Full-width list row (sidebar, file and picker rows). Only resets the button
 * and adds the hover and focus treatment; the caller's row classes keep their look.
 */
export const RowButton = forwardRef<HTMLButtonElement, RowButtonProps>(function RowButton({ selected = false, className, ...rest }, ref) {
  return <button ref={ref} type="button" aria-current={selected ? 'true' : undefined} data-selected={selected || undefined} className={cx('uam-row-button', className)} {...rest} />
})
