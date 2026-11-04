import { useId, useState, type ReactNode } from 'react'
import { ChevronRight } from 'lucide-react'
import { cx } from './cx'
import './disclosure.css'

export interface DisclosureProps {
  summary: ReactNode
  children: ReactNode
  defaultOpen?: boolean
  /** Controlled open state; pair with onOpenChange. */
  open?: boolean
  onOpenChange?: (open: boolean) => void
  className?: string
}

/** Animated replacement for <details>: content slides open from the summary and closes back into it. */
export function Disclosure({ summary, children, defaultOpen = false, open, onOpenChange, className }: DisclosureProps) {
  const [ownOpen, setOwnOpen] = useState(defaultOpen)
  const isOpen = open ?? ownOpen
  const id = useId()
  const toggle = () => { setOwnOpen(!isOpen); onOpenChange?.(!isOpen) }
  return (
    <div className={cx('uam-disclosure', className)} data-open={isOpen || undefined}>
      <button type="button" className="uam-disclosure__summary" aria-expanded={isOpen} aria-controls={id} onClick={toggle}>
        <ChevronRight size={14} aria-hidden className="uam-disclosure__chevron" />
        <span className="uam-disclosure__label">{summary}</span>
      </button>
      <div id={id} className="uam-disclosure__panel" aria-hidden={!isOpen || undefined} {...(!isOpen ? { inert: "" } : {})}>
        <div className="uam-disclosure__inner">{children}</div>
      </div>
    </div>
  )
}
