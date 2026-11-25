import type { HTMLAttributes } from 'react'
import { cx } from './cx'

export type BadgeProps = HTMLAttributes<HTMLSpanElement>

/**
 * Short read-only label beside a name, such as an agent's scope or role. Its text is part of the
 * surrounding accessible name; it is never a control and never the only cue for a state.
 */
export function Badge({ className, children, ...rest }: BadgeProps) {
  return <span className={cx('uam-badge', className)} {...rest}>{children}</span>
}
