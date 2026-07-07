import type { ReactNode } from 'react'
import { cx } from './cx'

export interface SectionHeaderProps {
  title: ReactNode
  /** Optional secondary text below the title. */
  description?: ReactNode
  /** Leading Lucide icon. */
  icon?: ReactNode
  /** Right-aligned actions (buttons, toggles). */
  actions?: ReactNode
  className?: string
}

/** Consistent section heading with optional icon, description, and actions. */
export function SectionHeader({ title, description, icon, actions, className }: SectionHeaderProps) {
  return (
    <div className={cx('uam-section-header', className)}>
      <div className="uam-section-header__lead">
        {icon ? <span className="uam-section-header__icon">{icon}</span> : null}
        <div className="uam-section-header__text">
          <div className="uam-section-header__title">{title}</div>
          {description ? <div className="uam-section-header__desc">{description}</div> : null}
        </div>
      </div>
      {actions ? <div className="uam-section-header__actions">{actions}</div> : null}
    </div>
  )
}
