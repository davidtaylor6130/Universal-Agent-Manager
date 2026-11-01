import { useRef, type ReactNode } from 'react'
import { cx } from './cx'
import { usePresence } from './Overlay'
import './reveal.css'

/**
 * Conditional content. Mounts while `open`, keeps its last open children through the exit, then unmounts.
 * Enter rises --sp-1 and fades in; exit fades out. Reduced motion and Motion Off skip movement.
 */
export function Reveal({ open, as: Tag = 'div', id, className, children }: { open: boolean; as?: 'div' | 'span'; id?: string; className?: string; children: ReactNode }) {
  const { mounted, closing } = usePresence(open)
  const lastChildren = useRef(children)
  if (open) lastChildren.current = children
  if (!mounted) return null
  return <Tag id={id} className={cx('uam-presence', Tag === 'span' && 'inline-flex', className)} data-state={closing ? 'closed' : 'open'}>{open ? children : lastChildren.current}</Tag>
}
