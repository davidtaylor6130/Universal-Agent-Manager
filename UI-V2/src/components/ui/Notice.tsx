import { useState, type ReactNode } from 'react'
import { Check, ChevronRight, CircleAlert, TriangleAlert, X } from 'lucide-react'
import { Button } from './Button'
import { IconButton } from './IconButton'
import { usePresence } from './Overlay'
import './notice-motion.css'

export type NoticeTone = 'info' | 'success' | 'warning' | 'error'

/** One bar: [icon] **title** message … [actions] [dismiss]. `details` is the only collapsible part (long logs/output), collapsed by default. */
export function Notice({
  tone = 'info',
  title,
  dismissLabel,
  dismissible = true,
  onDismiss,
  actions,
  details,
  children,
}: {
  tone?: NoticeTone
  title?: string
  /** Required when dismissible. */
  dismissLabel?: string
  dismissible?: boolean
  onDismiss?: () => void
  actions?: ReactNode
  details?: ReactNode
  children?: ReactNode
}) {
  const [visible, setVisible] = useState(true)
  const [showDetails, setShowDetails] = useState(false)
  const { mounted, closing } = usePresence(visible)
  if (!mounted) return null
  const color = tone === 'success' ? 'success' : tone === 'warning' ? 'warning' : tone === 'error' ? 'error' : 'accent'
  const heading = title ?? (tone === 'error' ? 'Error' : tone === 'warning' ? 'Warning' : tone === 'success' ? 'Completed' : 'Notice')
  // X is reserved for the dismiss control, so the error tone never uses it as its status icon.
  const StatusIcon = tone === 'success' ? Check : tone === 'warning' ? TriangleAlert : tone === 'error' ? CircleAlert : null
  return (
    <div
      role={tone === 'error' ? 'alert' : 'status'}
      data-state={closing ? 'closed' : 'open'}
      className="uam-notice mb-2 overflow-hidden rounded-md px-2.5 py-1.5 text-xs"
      style={{
        border: `1px solid color-mix(in srgb, var(--${color}) 42%, var(--border))`,
        background: `color-mix(in srgb, var(--${color}) 9%, var(--surface))`,
        color: 'var(--text)',
      }}
    >
      <div className="flex items-center gap-2">
        {StatusIcon && <StatusIcon size={14} className="shrink-0" style={{ color: `var(--${color})` }} aria-hidden />}
        <div className="min-w-0 flex-1" style={{ overflowWrap: 'anywhere' }}>
          <span className="font-semibold">{heading}</span>
          {children != null && <> {children}</>}
        </div>
        {details != null && (
          <Button type="button" asLink size="sm" aria-expanded={showDetails} onClick={() => setShowDetails((current) => !current)}>
            <ChevronRight size={12} className="inline-block transition-transform duration-150" style={{ transform: showDetails ? 'rotate(90deg)' : 'none' }} aria-hidden />
            Details
          </Button>
        )}
        {actions && <div className="flex shrink-0 flex-wrap items-center justify-end gap-1.5">{actions}</div>}
        {dismissible && dismissLabel && <IconButton
          icon={<X size={13} />}
          label={dismissLabel}
          size="sm"
          onClick={() => {
            setVisible(false)
            onDismiss?.()
          }}
        />}
      </div>
      {details != null && showDetails && (
        <div className="mt-1.5 border-t pt-1.5" style={{ borderColor: 'var(--border)', overflowWrap: 'anywhere' }}>{details}</div>
      )}
    </div>
  )
}
