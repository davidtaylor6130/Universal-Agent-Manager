import type { ReactNode } from 'react'
import { cx } from '../ui'
import './settings-shell.css'

/**
 * The one settings layout language, modelled on the MCP Servers page:
 * an uppercase section label with a one-line description, then a bordered
 * card of rows. A row puts its title and description on the left and its
 * control on the right; wide inputs use SettingsField instead. Descriptions are
 * one line: they truncate, and the full text is in the tooltip.
 */
export function SettingsSection({ title, description, actions, children, flush = false, className }: {
  title: string
  description?: ReactNode
  /** Right-aligned section actions (secondary buttons, icon buttons). */
  actions?: ReactNode
  children: ReactNode
  /** Remove card padding for edge-to-edge lists and tables. */
  flush?: boolean
  className?: string
}) {
  return (
    <section aria-label={title} className={cx('uam-set-section', className)}>
      <header className="uam-set-section__head">
        <div className="min-w-0">
          <h3 className="uam-set-section__title">{title}</h3>
          {description && <p className="uam-set-section__desc" title={typeof description === 'string' ? description : undefined}>{description}</p>}
        </div>
        {actions && <div className="uam-set-section__actions">{actions}</div>}
      </header>
      <div className={cx('uam-set-card', flush && 'uam-set-card--flush')}>{children}</div>
    </section>
  )
}

/** One setting: text on the left, its control on the right. Stacks on narrow widths. */
export function SettingRow({ title, description, control, icon, children, htmlFor, status }: {
  title: ReactNode
  description?: ReactNode
  control?: ReactNode
  /** Leading icon or provider logo. */
  icon?: ReactNode
  /** Extra content shown under the row (expanded detail, inline errors). */
  children?: ReactNode
  htmlFor?: string
  /** Muted status line under the description (what is happening, not what you set). */
  status?: ReactNode
}) {
  const Title = htmlFor ? 'label' : 'div'
  return (
    <div className="uam-set-row">
      <div className="uam-set-row__main">
        {icon && <span className="uam-set-row__icon">{icon}</span>}
        <div className="min-w-0">
          <Title className="uam-set-row__title" {...(htmlFor ? { htmlFor } : {})}>{title}</Title>
          {description && <div className="uam-set-row__desc" title={typeof description === 'string' ? description : undefined}>{description}</div>}
          {status && <div className="uam-set-row__status">{status}</div>}
        </div>
        {control && <div className="uam-set-row__control">{control}</div>}
      </div>
      {children && <div className="uam-set-row__extra">{children}</div>}
    </div>
  )
}

/** Label above a wide input (paths, text areas, multi-column forms). */
export function SettingsField({ label, hint, children, htmlFor }: { label: ReactNode; hint?: ReactNode; children: ReactNode; htmlFor?: string }) {
  return (
    <div className="uam-set-field">
      <label className="uam-set-field__label" htmlFor={htmlFor}>{label}</label>
      {children}
      {hint && <div className="uam-set-field__hint">{hint}</div>}
    </div>
  )
}

/** Animated placeholder while a settings page or section loads. */
export function SettingsLoading({ label = 'Loading settings', rows = 3 }: { label?: string; rows?: number }) {
  return (
    <div role="status" aria-busy="true" aria-label={label} className="uam-set-section">
      <div className="uam-set-skeleton uam-set-skeleton--label" />
      <div className="uam-set-card">
        {Array.from({ length: rows }, (_, index) => (
          <div key={index} className="uam-set-row" aria-hidden>
            <div className="uam-set-row__main">
              <div className="min-w-0 flex-1 grid gap-1.5">
                <div className="uam-set-skeleton" style={{ width: `${38 + (index * 17) % 30}%` }} />
                <div className="uam-set-skeleton uam-set-skeleton--thin" style={{ width: `${55 + (index * 23) % 30}%` }} />
              </div>
              <div className="uam-set-skeleton uam-set-skeleton--control" />
            </div>
          </div>
        ))}
      </div>
      <span className="sr-only">{label}…</span>
    </div>
  )
}
