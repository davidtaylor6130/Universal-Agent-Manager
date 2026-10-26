import { useId, useLayoutEffect, useRef } from 'react'
import type { ReactNode } from 'react'
import { Button, cx } from '../ui'
import type { ButtonProps } from '../ui'
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

const CONTROL_GROUPS = '[role="group"], [role="radiogroup"]'
const DESCRIBED_CONTROLS = `input, select, textarea, button, ${CONTROL_GROUPS}`

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
  const descriptionId = useId()
  const statusId = useId()
  const controlRef = useRef<HTMLDivElement>(null)
  // Controls are arbitrary nodes, so the row links its description/status to each focusable control (or its group) after render.
  useLayoutEffect(() => {
    const ids = [description ? descriptionId : '', status ? statusId : ''].filter(Boolean)
    const host = controlRef.current
    if (!host || !ids.length) return
    for (const element of host.querySelectorAll<HTMLElement>(DESCRIBED_CONTROLS)) {
      const group = element.parentElement?.closest(CONTROL_GROUPS)
      if (group && host.contains(group)) continue
      const current = element.getAttribute('aria-describedby')?.split(/\s+/).filter(Boolean) ?? []
      const next = [...new Set([...current, ...ids])].join(' ')
      if (next !== current.join(' ')) element.setAttribute('aria-describedby', next)
    }
  })
  return (
    <div className="uam-set-row">
      <div className="uam-set-row__main">
        {icon && <span className="uam-set-row__icon">{icon}</span>}
        <div className="min-w-0">
          <Title className="uam-set-row__title" {...(htmlFor ? { htmlFor } : {})}>{title}</Title>
          {description && <div id={descriptionId} className="uam-set-row__desc" title={typeof description === 'string' ? description : undefined}>{description}</div>}
          {status && <div id={statusId} className="uam-set-row__status">{status}</div>}
        </div>
        {control && <div ref={controlRef} className="uam-set-row__control">{control}</div>}
      </div>
      {children && <div className="uam-set-row__extra">{children}</div>}
    </div>
  )
}

/** Settings save action: busy while pending, with the pending label announced politely. */
export function SaveButton({ saving, pendingLabel = 'Saving…', variant = 'primary', size = 'sm', children, ...props }: ButtonProps & { saving: boolean; pendingLabel?: string }) {
  return <>
    <Button variant={variant} size={size} aria-busy={saving || undefined} {...props}>{saving ? pendingLabel : children}</Button>
    <span className="sr-only" role="status" aria-live="polite">{saving ? pendingLabel : ''}</span>
  </>
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
