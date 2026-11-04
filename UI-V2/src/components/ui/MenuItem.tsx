import { forwardRef, useId } from 'react'
import type { ButtonHTMLAttributes, ReactNode } from 'react'
import { Check } from 'lucide-react'
import { cx } from './cx'

export interface MenuItemProps extends Omit<ButtonHTMLAttributes<HTMLButtonElement>, 'role' | 'children' | 'onSelect' | 'disabled'> {
  icon?: ReactNode
  label: ReactNode
  /** Muted secondary text; truncates with an ellipsis. */
  detail?: ReactNode
  /** `stacked` puts the detail on a second line under the label. */
  detailLayout?: 'inline' | 'stacked'
  /** Pill or badge at the trailing edge, before the shortcut. */
  trailing?: ReactNode
  /** Keyboard shortcut hint, shown at the trailing edge. */
  shortcut?: string
  /** Defined = checkable item (menuitemradio). `true` shows a check icon. */
  selected?: boolean
  /** Defined = checkable item (menuitemcheckbox) with aria-checked. Takes precedence over `selected`. */
  checked?: boolean
  danger?: boolean
  disabled?: boolean
  /** Use the native disabled attribute (not focusable) instead of aria-disabled. */
  nativeDisabled?: boolean
  /** Explains a disabled item; announced with the item. */
  disabledReason?: string
  onSelect?: () => void
}

/**
 * Row inside menus, context menus and the command palette. Disabled items stay
 * focusable (aria-disabled) so the reason can be announced, unless `nativeDisabled`.
 * Look matches the shared row-menu item (`.uam-menu-item` in index.css).
 */
export const MenuItem = forwardRef<HTMLButtonElement, MenuItemProps>(function MenuItem(
  { icon, label, detail, detailLayout = 'inline', trailing, shortcut, selected, checked, danger = false, disabled = false, nativeDisabled = false, disabledReason, className, onSelect, onClick, title, 'aria-label': ariaLabel, 'aria-describedby': describedBy, ...rest },
  ref
) {
  const reasonId = useId()
  const checkable = selected !== undefined || checked !== undefined
  const isChecked = checked !== undefined ? checked : selected
  const role = checked !== undefined ? 'menuitemcheckbox' : selected !== undefined ? 'menuitemradio' : 'menuitem'
  const stacked = detailLayout === 'stacked' && Boolean(detail)
  const labelNode = <span className="uam-menu-item__label">{label}</span>
  const detailNode = detail && <span className="uam-menu-item__detail">{detail}</span>
  return (
    <button
      {...rest}
      ref={ref}
      type="button"
      role={role}
      aria-label={ariaLabel ?? (typeof label === 'string' ? label : undefined)}
      aria-checked={checkable ? isChecked : undefined}
      aria-disabled={!nativeDisabled && disabled || undefined}
      disabled={nativeDisabled ? disabled : undefined}
      aria-describedby={disabled && disabledReason ? reasonId : describedBy}
      title={disabled && disabledReason ? disabledReason : title}
      className={cx('uam-menu-item', danger && 'uam-menu-item--danger', isChecked && 'is-selected', className)}
      onClick={(event) => {
        if (disabled) return
        onClick?.(event)
        onSelect?.()
      }}
    >
      {checkable
        ? <span className="uam-menu-item__icon" aria-hidden>{isChecked && <Check size={14} />}</span>
        : icon && <span className="uam-menu-item__icon" aria-hidden>{icon}</span>}
      {stacked
        ? <span className="uam-menu-item__text">{labelNode}{detailNode}</span>
        : <>{labelNode}{detailNode}</>}
      {trailing && <span className="uam-menu-item__trailing">{trailing}</span>}
      {shortcut && <kbd className="uam-menu-kbd">{shortcut}</kbd>}
      {disabled && disabledReason && <span id={reasonId} hidden>{disabledReason}</span>}
    </button>
  )
})
