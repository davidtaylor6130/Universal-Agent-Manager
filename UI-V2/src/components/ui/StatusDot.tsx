import { cx } from './cx'

export type StatusTone = 'neutral' | 'success' | 'warning' | 'error' | 'info' | 'accent'

export interface StatusDotProps {
  tone?: StatusTone
  /** Soft pulsing animation for live/active states. */
  pulse?: boolean
  size?: number
  className?: string
}

/** Small semantic status indicator dot. Pair with a Tooltip for meaning. */
export function StatusDot({ tone = 'neutral', pulse = false, size = 8, className }: StatusDotProps) {
  return (
    <span
      className={cx('uam-status-dot', `uam-status-dot--${tone}`, pulse && 'is-pulse', className)}
      style={{ width: size, height: size }}
      aria-hidden
    />
  )
}
