import { describe, expect, it } from 'vitest'
import { notificationTimeGroup, relativeNotificationTime, retainNotificationTimes } from './notificationTime'

describe('notification timestamps', () => {
  it('retains first observation and recorded failure time until the source disappears', () => {
    const recorded = '2026-10-22T09:00:00.000Z'
    const first = retainNotificationTimes(new Map(), [{ id: 'activity' }, { id: 'failure', time: recorded }, { id: 'invalid', time: 'unknown' }], 1000)
    expect([...first]).toEqual([['activity', 1000], ['failure', Date.parse(recorded)], ['invalid', 1000]])
    const unchanged = retainNotificationTimes(first, [{ id: 'failure', time: recorded }, { id: 'activity' }, { id: 'new' }], 5000)
    expect([...unchanged]).toEqual([['failure', Date.parse(recorded)], ['activity', 1000], ['new', 5000]])
    expect(unchanged.has('invalid')).toBe(false)
    const resolved = retainNotificationTimes(unchanged, [{ id: 'new' }], 6000)
    const returned = retainNotificationTimes(resolved, [{ id: 'new' }, { id: 'activity' }], 7000)
    expect(returned.get('activity')).toBe(7000)
    expect(returned.get('new')).toBe(5000)
    expect([...first]).toHaveLength(3)
  })

  it('uses local midnight rather than a rolling 24-hour boundary', () => {
    const midnight = new Date(2026, 9, 23).getTime()
    expect(notificationTimeGroup(midnight - 1, midnight + 1000)).toBe('Earlier')
    expect(notificationTimeGroup(midnight, midnight + 1000)).toBe('Today')
    expect(notificationTimeGroup(midnight + 1000, midnight + 23 * 60 * 60_000)).toBe('Today')
    // A future recorded time must not be described as an earlier notice.
    expect(notificationTimeGroup(midnight + 2 * 24 * 60 * 60_000, midnight)).toBe('Today')
  })

  it.each([
    [0, 'Just now'], [59_999, 'Just now'], [60_000, '1 minute ago'], [120_000, '2 minutes ago'],
    [3_599_999, '59 minutes ago'], [3_600_000, '1 hour ago'], [7_200_000, '2 hours ago'],
    [86_399_999, '23 hours ago'], [86_400_000, '1 day ago'], [172_800_000, '2 days ago'],
    [-60_000, 'Just now'],
  ])('formats elapsed %i ms without negative ages', (elapsed, expected) => {
    expect(relativeNotificationTime(0, elapsed)).toBe(expected)
  })
})
