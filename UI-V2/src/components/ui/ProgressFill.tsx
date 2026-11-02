import type { CSSProperties, ReactNode } from 'react'
import { cx } from './cx'

export interface ProgressFillProps {
  /** Accessible name for the progressbar or meter. */
  label: string
  /** Known amount: the fill grows left to right to value / max. Omit when no amount is known. */
  value?: number
  /** Scale for value (default 100, a percentage). */
  max?: number
  /** No percentage is known: the fill sweeps. Also implied when value is missing. */
  indeterminate?: boolean
  /** False keeps the same element (so children never remount) with no fill and no progress role. Default true. */
  active?: boolean
  /**
   * fill (default): the item doing the work is the track. meter: a standalone bar for a measured quantity with a real
   * limit (role meter), with an optional marker tick, a tone and spoken value text. Children are not rendered.
   */
  variant?: 'fill' | 'meter'
  /** Meter only: a thin tick at this value on the same scale, such as the share of time already passed. */
  marker?: number | null
  /** Meter only: fill colour. Neutral by default. */
  tone?: 'neutral' | 'warning' | 'error'
  /** Meter only: aria-valuetext, read instead of the bare number. */
  valueText?: string
  /** Meter only: lg is the 10px allowance bar; md (default) the 6px bar. */
  size?: 'md' | 'lg'
  /** Meter only: equal notches across the track, such as 7 day marks on a weekly window. */
  segments?: number
  /** Meter only: a caret under the track at this value on the same scale, labelled (decorative; say it in valueText too). */
  projection?: { at: number; label: string; tone?: 'warning' | 'error' } | null
  className?: string
  children?: ReactNode
}

/** The item doing the work is the progress track: its background fills in --accent-dim and text stays on top. */
export function ProgressFill({ label, value, max = 100, indeterminate = false, active = true, variant = 'fill', marker, tone = 'neutral', valueText, size = 'md', segments, projection, className, children }: ProgressFillProps) {
  const pct = !indeterminate && typeof value === 'number' && Number.isFinite(value) && max > 0 ? Math.min(max, Math.max(0, value)) : undefined
  const busy = pct === undefined
  if (variant === 'meter') {
    // Unknown or reading: the same empty track with no role (the caller states why in text); reading sweeps inside it.
    const meterClass = cx('uam-meter', size === 'lg' && 'uam-meter--lg', className)
    if (!active || busy) return <span aria-hidden className={meterClass} data-indeterminate={(active && busy) || undefined} />
    const share = (at: number | null | undefined) => typeof at === 'number' && Number.isFinite(at) ? Math.min(max, Math.max(0, at)) / max : undefined
    const tick = share(marker)
    const caret = projection ? share(projection.at) : undefined
    return (
      <span role="meter" aria-label={label} aria-valuemin={0} aria-valuemax={max} aria-valuenow={pct} aria-valuetext={valueText}
        data-tone={tone === 'neutral' ? undefined : tone} className={meterClass} style={{ '--progress': pct / max } as CSSProperties}>
        {segments && segments > 1 ? <span aria-hidden className="uam-meter__segments" style={{ '--segments': Math.round(segments) } as CSSProperties} /> : null}
        {tick !== undefined && <span className="uam-meter__marker" style={{ '--marker': tick } as CSSProperties} />}
        {/* Past about 60% the label flips to the caret's left so it stays inside the track's width. */}
        {caret !== undefined && <span aria-hidden className="uam-meter__projection" data-tone={projection!.tone ?? 'warning'} data-flip={caret > 0.6 || undefined}
          style={{ '--at': caret } as CSSProperties}>{projection!.label}</span>}
      </span>
    )
  }
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
