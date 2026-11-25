import { useAppStore } from '../../store/useAppStore'
import { hostLoadSummary, hostSlotsLabel, hostUsageSummary } from '../../types/hostLoad'
import { Tooltip } from '../ui'

/**
 * A chat's computer with its live use, for example "Remote · Homelab · 3/8". The slots part appears once native
 * reports figures for that computer. The chip is then a focusable button named by its visible text, so keyboard
 * users can reach the tooltip, which adds the use, load, memory and process counts as its description.
 */
export function HostLoadChip({ hostId, label, testId }: { hostId: string; label: string; testId?: string }) {
  const figures = useAppStore((state) => state.executionHostLoad.hosts[hostId])
  const slots = hostSlotsLabel(figures)
  if (!figures) return <span data-testid={testId} className="shrink-0">{label}</span>
  return <Tooltip label={`${hostUsageSummary(figures)} · ${hostLoadSummary(figures)}`}>
    <button type="button" data-testid={testId} className="uam-host-chip shrink-0">
      {label}{slots && <span className="uam-host-chip__slots"> · {slots}</span>}
    </button>
  </Tooltip>
}
