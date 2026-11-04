import { reportAppFailure } from './appFailure'

export type BulkTarget = { id: string; label: string }

/** Runs the action for every target in order; every failure is reported by name in one shared notification. Returns succeeded ids. */
export async function runForEach(targets: BulkTarget[], action: (target: BulkTarget) => Promise<string | null>, failureMessage: string): Promise<string[]> {
  const succeeded: string[] = []
  const failed: string[] = []
  for (const target of targets) {
    let error: string | null
    try { error = await action(target) }
    catch (cause) { error = cause instanceof Error && cause.message.trim() ? cause.message : 'The request failed.' }
    if (error === null) succeeded.push(target.id)
    else failed.push(`“${target.label}”: ${error}`)
  }
  if (failed.length > 0) reportAppFailure(`${failureMessage} (${failed.length} of ${targets.length})`, failed.join(' · '))
  return succeeded
}
