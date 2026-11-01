import type { CSSProperties, ReactNode } from 'react'
import { cx } from './cx'

export interface ProgressFillProps {
  /** Accessible name for the progressbar. */
  label: string
  /** Known amount: the fill grows left to right to value / max. Omit when no amount is known. */
  value?: number
  /** Scale for value (default 100, a percentage). */
  max?: number
  /** No percentage is known: the fill sweeps. Also implied when value is missing. */
  indeterminate?: boolean
  /** False keeps the same element (so children never remount) with no fill and no progress role. Default true. */
  active?: boolean
  className?: string
  children?: ReactNode
}

/** The item doing the work is the progress track: its background fills in --accent-dim and text stays on top. */
export function ProgressFill({ label, value, max = 100, indeterminate = false, active = true, className, children }: ProgressFillProps) {
  const pct = !indeterminate && typeof value === 'number' && Number.isFinite(value) && max > 0 ? Math.min(max, Math.max(0, value)) : undefined
  const busy = pct === undefined
  if (!active) return <div className={className}>{children}</div>
  return (
    <div
      role="progressbar"
      aria-label={label}
      aria-valuemin={busy ? undefined : 0}
      aria-valuemax={busy ? undefined : max}
      aria-valuenow={pct}
      aria-busy={busy || undefined}
      data-indeterminate={busy || undefined}
      className={cx('uam-progress-fill', className)}
      style={busy ? undefined : ({ '--progress': pct / max } as CSSProperties)}
    >
      {children}
    </div>
  )
}
