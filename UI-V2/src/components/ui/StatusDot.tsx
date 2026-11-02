import type { CSSProperties } from 'react'
import { Check, X } from 'lucide-react'
import { cx } from './cx'

export type StatusTone = 'neutral' | 'success' | 'warning' | 'error' | 'info' | 'accent'

export interface StatusDotProps {
  tone?: StatusTone
  /** Soft pulsing animation for live/active states. */
  pulse?: boolean
  size?: number
  className?: string
}

/** Small semantic status indicator. Success and error render a tick or cross (never colour alone), boxed in a rounded square inside Settings only; other tones render a dot. Pair with a Tooltip for meaning. */
export function StatusDot({ tone = 'neutral', pulse = false, size = 8, className }: StatusDotProps) {
  if (tone === 'success' || tone === 'error') {
    const Icon = tone === 'success' ? Check : X
    return <span className={cx('uam-status-box', `uam-status-box--${tone}`, className)} style={{ '--status-box-size': `${size + 8}px` } as CSSProperties} aria-hidden>
      <Icon size={size + 4} strokeWidth={2.5} aria-hidden />
    </span>
  }
  return (
    <span
      className={cx('uam-status-dot', `uam-status-dot--${tone}`, pulse && 'is-pulse', className)}
      style={{ width: size, height: size }}
      aria-hidden
    />
  )
}
