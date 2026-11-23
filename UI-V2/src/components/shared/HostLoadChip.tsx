import { useAppStore } from '../../store/useAppStore'
import { hostLoadSummary, hostSlotsLabel, hostUsageSummary } from '../../types/hostLoad'
import { Tooltip } from '../ui'

/**
 * A chat's computer with its live use, for example "Remote · Homelab · 3/8". The slots part appears once native
 * reports figures for that computer; the tooltip and accessible name add load, memory and process counts.
 */
export function HostLoadChip({ hostId, label, testId }: { hostId: string; label: string; testId?: string }) {
  const figures = useAppStore((state) => state.executionHostLoad.hosts[hostId])
  const slots = hostSlotsLabel(figures)
  if (!figures) return <span data-testid={testId} className="shrink-0">{label}</span>
  const detail = `${hostUsageSummary(figures)} · ${hostLoadSummary(figures)}`
  return <Tooltip label={detail}>
    <span data-testid={testId} tabIndex={0} aria-label={`${label}: ${detail}`} className="uam-host-chip shrink-0">
      {label}{slots && <span className="uam-host-chip__slots"> · {slots}</span>}
    </span>
  </Tooltip>
}
