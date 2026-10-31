import { useState, type ReactNode } from 'react'
import { Check, ChevronRight, TriangleAlert, X } from 'lucide-react'
import { IconButton } from './IconButton'
import { usePresence } from './Overlay'
import './notice-motion.css'

export type NoticeTone = 'info' | 'success' | 'warning' | 'error'

export function Notice({
  tone = 'info',
  title,
  dismissLabel,
  dismissible = true,
  onDismiss,
  actions,
  children,
}: {
  tone?: NoticeTone
  title?: string
  dismissLabel: string
  dismissible?: boolean
  onDismiss?: () => void
  actions?: ReactNode
  children: ReactNode
}) {
  const [visible, setVisible] = useState(true)
  const [expanded, setExpanded] = useState(true)
  const { mounted, closing } = usePresence(visible)
  if (!mounted) return null
  const color = tone === 'success' ? 'success' : tone === 'warning' ? 'warning' : tone === 'error' ? 'error' : 'accent'
  const heading = title ?? (tone === 'error' ? 'Error' : tone === 'warning' ? 'Warning' : tone === 'success' ? 'Completed' : 'Notice')
  const StatusIcon = tone === 'success' ? Check : tone === 'warning' ? TriangleAlert : tone === 'error' ? X : null
  return (
    <div
      role={tone === 'error' ? 'alert' : 'status'}
      data-state={closing ? 'closed' : 'open'}
      className="uam-notice mb-2 overflow-hidden rounded-md text-xs"
      style={{
        border: `1px solid color-mix(in srgb, var(--${color}) 42%, var(--border))`,
        background: `color-mix(in srgb, var(--${color}) 9%, var(--surface))`,
        color: 'var(--text)',
      }}
    >
      <div className="flex items-center gap-1 px-2 py-1.5" style={{ borderBottom: expanded ? '1px solid var(--border)' : 0 }}>
        <button
          type="button"
          className="flex min-w-0 flex-1 items-center gap-1.5 text-left font-semibold"
          aria-expanded={expanded}
          onClick={() => setExpanded((current) => !current)}
        >
          <ChevronRight size={13} className="shrink-0 transition-transform duration-150" style={{ transform: expanded ? 'rotate(90deg)' : 'none', color: `var(--${color})` }} aria-hidden />
          {StatusIcon && <StatusIcon size={13} className="shrink-0" style={{ color: `var(--${color})` }} aria-hidden />}
          <span className="truncate">{heading}</span>
        </button>
        {dismissible && <IconButton
          icon={<X size={13} />}
          label={dismissLabel}
          size="sm"
          onClick={() => {
            setVisible(false)
            onDismiss?.()
          }}
        />}
      </div>
      {expanded && (
        <div className="grid gap-2 px-3 py-2" style={{ overflowWrap: 'anywhere' }}>
          <div>{children}</div>
          {actions && <div className="flex flex-wrap justify-end gap-2">{actions}</div>}
        </div>
      )}
    </div>
  )
}
