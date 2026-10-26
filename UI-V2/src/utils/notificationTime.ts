/** Existing notice identity remains authoritative; time is display metadata only. */
export type NotificationTimeSource = { id: string; time?: string }

export function retainNotificationTimes(previous: ReadonlyMap<string, number>, sources: readonly NotificationTimeSource[], observedAt: number): ReadonlyMap<string, number> {
  return new Map(sources.map(({ id, time }) => {
    const recordedAt = time ? Date.parse(time) : NaN
    return [id, Number.isFinite(recordedAt) ? recordedAt : previous.get(id) ?? observedAt]
  }))
}

export function notificationTimeGroup(time: number, now: number): 'Today' | 'Earlier' {
  const midnight = new Date(now)
  midnight.setHours(0, 0, 0, 0)
  return time >= midnight.getTime() ? 'Today' : 'Earlier'
}

export function relativeNotificationTime(time: number, now: number): string {
  const minutes = Math.floor(Math.max(0, now - time) / 60_000)
  if (minutes === 0) return 'Just now'
  if (minutes < 60) return `${minutes} minute${minutes === 1 ? '' : 's'} ago`
  const hours = Math.floor(minutes / 60)
  if (hours < 24) return `${hours} hour${hours === 1 ? '' : 's'} ago`
  const days = Math.floor(hours / 24)
  return `${days} day${days === 1 ? '' : 's'} ago`
}
