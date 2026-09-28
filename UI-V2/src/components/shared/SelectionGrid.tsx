import type { ReactNode } from 'react'
import { StatusIndicator } from './StatusIndicator'

/** Icon and name choices shared by host, provider and setup workflows. */
export function SelectionGrid({ label, options, value, onChange }: {
  label: string
  options: { id: string; label: string; icon: ReactNode; disabled?: boolean; issues?: string[] }[]
  value: string
  onChange: (id: string) => void
}) {
  return <div role="group" aria-label={label} className="uam-selection-grid">
    {options.map(option => <button type="button" key={option.id}
      aria-label={option.label} aria-pressed={option.id === value} disabled={option.disabled}
      onClick={() => onChange(option.id)} className="uam-selection-option">
      <span className="uam-selection-icon">{option.icon}</span>
      <span className="max-w-full truncate">{option.label}</span>
      {!!option.issues?.length && <span className="uam-selection-status"><StatusIndicator issues={option.issues} focusable={false} /></span>}
    </button>)}
  </div>
}
