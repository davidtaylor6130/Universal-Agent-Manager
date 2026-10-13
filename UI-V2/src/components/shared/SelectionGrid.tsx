import type { ReactNode } from 'react'
import { StatusIndicator } from './StatusIndicator'
import { StatusDot, type StatusTone } from '../ui'

/** Icon and name choices shared by host, provider and setup workflows. `detail` (mono) and `tone` are optional. */
export function SelectionGrid({ label, options, value, onChange }: {
  label: string
  options: { id: string; label: string; icon: ReactNode; disabled?: boolean; issues?: string[]; detail?: string; tone?: StatusTone }[]
  value: string
  onChange: (id: string) => void
}) {
  return <div role="group" aria-label={label} className="uam-selection-grid">
    {options.map(option => <button type="button" key={option.id}
      aria-label={option.label} aria-pressed={option.id === value} disabled={option.disabled}
      onClick={() => onChange(option.id)} className="uam-selection-option">
      <span className="uam-selection-icon">{option.icon}</span>
      <span className="flex max-w-full items-center gap-1.5 truncate">
        {option.tone && <StatusDot tone={option.tone} size={6} />}
        <span className="truncate">{option.label}</span>
      </span>
      {option.detail && <span className="max-w-full truncate font-mono" style={{ fontSize: 'var(--fs-xs)', color: 'var(--text-3)' }}>{option.detail}</span>}
      {!!option.issues?.length && <span className="uam-selection-status"><StatusIndicator issues={option.issues} focusable={false} /></span>}
    </button>)}
  </div>
}
