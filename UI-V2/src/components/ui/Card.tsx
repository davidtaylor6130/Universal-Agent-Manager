import type { HTMLAttributes, ReactNode } from 'react'
import { cx } from './cx'

export interface CardProps extends HTMLAttributes<HTMLDivElement> {
  /** Elevation level: 0 flat (border only), 1 resting card, 2 raised. */
  elevation?: 0 | 1 | 2
  /** Remove inner padding (for edge-to-edge content like lists). */
  flush?: boolean
  children: ReactNode
}

/** Surface container for the dashboard layout. Groups related content. */
export function Card({ elevation = 1, flush = false, className, children, ...rest }: CardProps) {
  return (
    <div
      className={cx('uam-card', `uam-card--elev-${elevation}`, flush && 'uam-card--flush', className)}
      {...rest}
    >
      {children}
    </div>
  )
}
