import { Check, TriangleAlert } from 'lucide-react'
import { Tooltip } from '../ui'

/** Shared ready/warning state, with details available on hover and keyboard focus. */
export function StatusIndicator({ issues = [], okLabel = 'Ready', focusable = true }: {
  issues?: string[]
  okLabel?: string
  focusable?: boolean
}) {
  const warnings = [...new Set(issues.filter(Boolean))]
  const label = warnings.length ? warnings.join('\n') : okLabel
  return <Tooltip label={<span className="whitespace-pre-line">{label}</span>}>
    <span tabIndex={focusable ? 0 : undefined} role="img" aria-label={label} className="uam-status-indicator" style={{ color: warnings.length ? 'var(--yellow)' : 'var(--green)' }}>
      {warnings.length > 1
        ? <span className="uam-status-count">{warnings.length}</span>
        : warnings.length ? <TriangleAlert size={16} aria-hidden /> : <Check size={16} aria-hidden />}
    </span>
  </Tooltip>
}
