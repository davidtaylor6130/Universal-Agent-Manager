export type SubagentViewportVisit = Readonly<{ page: number | null; top: number; followTail: boolean }>

// Window-local reading positions only; never retain transcripts or runtime controls.
const visits = new Map<string, SubagentViewportVisit>()

export const readSubagentViewportVisit = (key: string) => key ? visits.get(key) : undefined

export function rememberSubagentViewportVisit(key: string, visit: SubagentViewportVisit) {
  if (!key || !Number.isFinite(visit.top)) return
  visits.delete(key)
  visits.set(key, { page: visit.page, top: Math.max(0, visit.top), followTail: visit.followTail })
  if (visits.size > 64) visits.delete(visits.keys().next().value!)
}
