import { useId } from 'react'
import type { ReactNode } from 'react'
import { Check } from 'lucide-react'
import { cx } from './cx'

export interface MenuItemProps {
  icon?: ReactNode
  label: ReactNode
  /** Muted secondary text; truncates with an ellipsis. */
  detail?: ReactNode
  /** Keyboard shortcut hint, shown at the trailing edge. */
  shortcut?: string
  /** Defined = checkable item (menuitemradio). `true` shows a check icon. */
  selected?: boolean
  danger?: boolean
  disabled?: boolean
  /** Explains a disabled item; announced with the item. */
  disabledReason?: string
  className?: string
  onSelect?: () => void
}

/**
 * Row inside menus, context menus and the command palette. Disabled items stay
 * focusable (aria-disabled) so the reason can be announced. Look matches the
 * shared row-menu item (`.uam-menu-item` in index.css).
 */
export function MenuItem({ icon, label, detail, shortcut, selected, danger = false, disabled = false, disabledReason, className, onSelect }: MenuItemProps) {
  const reasonId = useId()
  const checkable = selected !== undefined
  return (
    <button
      type="button"
      role={checkable ? 'menuitemradio' : 'menuitem'}
      aria-label={typeof label === 'string' ? label : undefined}
      aria-checked={checkable ? selected : undefined}
      aria-disabled={disabled || undefined}
      aria-describedby={disabled && disabledReason ? reasonId : undefined}
      title={disabled ? disabledReason : undefined}
      className={cx('uam-menu-item', danger && 'uam-menu-item--danger', selected && 'is-selected', className)}
      onClick={() => { if (!disabled) onSelect?.() }}
    >
      {checkable
        ? <span className="uam-menu-item__icon" aria-hidden>{selected && <Check size={14} />}</span>
        : icon && <span className="uam-menu-item__icon" aria-hidden>{icon}</span>}
      <span className="uam-menu-item__label">{label}</span>
      {detail && <span className="uam-menu-item__detail">{detail}</span>}
      {shortcut && <kbd className="uam-menu-kbd">{shortcut}</kbd>}
      {disabled && disabledReason && <span id={reasonId} hidden>{disabledReason}</span>}
    </button>
  )
}
