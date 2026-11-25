import { describe, expect, it } from 'vitest'
import { EMPTY_HOST_LOAD, hostChipLabel, hostLoadSummary, hostSlotsLabel, hostUsageSummary, sameRoutingWaitHosts, sanitizeExecutionHostLoad, sanitizeRoutingWaitHosts, type HostLoadFigures } from './hostLoad'
import { sanitizeCppAppState, sanitizeCppChat, sanitizeCppStatePatch } from '../store/cpp/sanitizers'
import { sessionFromCppChat } from '../store/cpp/reconcile'

const known: HostLoadFigures = { status: 'known', cores: 8, loadAverage1m: 2.5, cpuPercent: null, memoryAvailableBytes: 4 * 1024 ** 3, memoryTotalBytes: 16 * 1024 ** 3, processes: 3, inUse: 3, waiting: 2, limit: 8 }

describe('host load figures', () => {
  it('keeps figures only for a current reading, so unknown load never reads as idle', () => {
    const raw = { hosts: {
      lab: { ...known, status: 'known' },
      old: { ...known, status: 'unsupported', cores: 0, loadAverage1m: 0, memoryAvailableBytes: 0, processes: 0 },
      gone: { ...known, status: 'unreachable' },
      local: { status: 'notSampled', processes: 2, inUse: 1, waiting: 0, limit: null },
      odd: { status: 'surprising', cores: -1, cpuPercent: 140, inUse: 'many', limit: 1.5 },
    }, unassignedWaiting: 4 }
    const load = sanitizeExecutionHostLoad(raw)
    expect(load.hosts.lab).toEqual(known)
    for (const id of ['old', 'gone']) {
      expect(load.hosts[id]).toMatchObject({ cores: null, loadAverage1m: null, memoryAvailableBytes: null, processes: null, inUse: 3, waiting: 2, limit: 8 })
    }
    // This computer's own process count is counted by the app, so it is known without a reading.
    expect(load.hosts.local).toMatchObject({ status: 'notSampled', processes: 2, inUse: 1, limit: null })
    expect(load.hosts.odd).toMatchObject({ status: 'notSampled', cores: null, cpuPercent: null, inUse: 0, limit: null })
    expect(load.unassignedWaiting).toBe(4)
    expect(sanitizeExecutionHostLoad(null)).toBe(EMPTY_HOST_LOAD)
    expect(sanitizeExecutionHostLoad({ hosts: [] })).toBe(EMPTY_HOST_LOAD)
  })

  it('formats the chip, usage and load lines and says when load is unknown', () => {
    expect(hostSlotsLabel(known)).toBe('3/8')
    expect(hostChipLabel('Remote · Lab', known)).toBe('Remote · Lab · 3/8')
    expect(hostChipLabel('Remote · Lab', undefined)).toBe('Remote · Lab')
    expect(hostSlotsLabel({ ...known, limit: null })).toBe('3 in use')
    expect(hostUsageSummary(known)).toBe('3 of 8 in use · 2 waiting')
    expect(hostUsageSummary({ ...known, waiting: 0, limit: null })).toBe('3 in use')
    expect(hostLoadSummary(known)).toBe('Load 2.5 on 8 cores · 4.0 GB free · 3 processes')
    expect(hostLoadSummary({ ...known, loadAverage1m: null, cpuPercent: 37.4, cores: 1, processes: 1 })).toBe('CPU 37% on 1 core · 4.0 GB free · 1 process')
    expect(hostLoadSummary({ ...known, status: 'unsupported', loadAverage1m: null, memoryAvailableBytes: null, cores: null, processes: null })).toBe('Load unknown: update this computer’s helper')
    expect(hostLoadSummary({ ...known, status: 'stale', processes: null })).toBe('Load unknown: no recent reading')
    expect(hostLoadSummary(undefined)).toBe('Load not read')
  })

  it('reads per-computer wait reasons and reconciles them like other routing fields', () => {
    expect(sanitizeRoutingWaitHosts([{ hostId: 'a', reason: 'full' }, { hostId: 'b', reason: 'sleepy' }, { hostId: '', reason: 'off' }, 'x'])).toEqual([{ hostId: 'a', reason: 'full' }])
    expect(sanitizeRoutingWaitHosts(undefined)).toBeUndefined()
    expect(sameRoutingWaitHosts([{ hostId: 'a', reason: 'full' }], [{ hostId: 'a', reason: 'full' }])).toBe(true)
    expect(sameRoutingWaitHosts([{ hostId: 'a', reason: 'full' }], [{ hostId: 'a', reason: 'unreachable' }])).toBe(false)
    expect(sameRoutingWaitHosts(undefined, [])).toBe(true)

    const raw = { id: 'chat', title: 'Chat', createdAt: '2026-01-01T00:00:00Z', updatedAt: '2026-01-01T00:00:00Z', routingMode: 'automatic', routingWaitReason: 'No computer can take this chat yet: 1 full.', routingWaitHosts: [{ hostId: 'lab', reason: 'full' }] }
    const first = sessionFromCppChat(sanitizeCppChat(raw)!, undefined, [])
    expect(first.routingWaitHosts).toEqual([{ hostId: 'lab', reason: 'full' }])
    expect(sessionFromCppChat(sanitizeCppChat({ ...raw })!, first, [])).toBe(first)
    const changed = sessionFromCppChat(sanitizeCppChat({ ...raw, routingWaitHosts: [{ hostId: 'lab', reason: 'unreachable' }] })!, first, [])
    expect(changed).not.toBe(first)
    expect(changed.routingWaitHosts).toEqual([{ hostId: 'lab', reason: 'unreachable' }])
  })

  it('carries load in full states and patches, splitting pool use by computer', () => {
    const state = sanitizeCppAppState({ chats: [], providers: [], settings: {}, executionHostLoad: { hosts: { lab: known }, unassignedWaiting: 1 },
      computePoolUsage: { pool: { inUse: 2, waiting: 1, hosts: { local: { inUse: 1, waiting: 0 }, lab: { inUse: 1, waiting: 1 }, bad: 3 } } } })
    expect(state?.executionHostLoad?.hosts.lab).toEqual(known)
    expect(state?.computePoolUsage?.pool).toEqual({ inUse: 2, waiting: 1, hosts: { local: { inUse: 1, waiting: 0 }, lab: { inUse: 1, waiting: 1 } } })
    expect(sanitizeCppStatePatch({ executionHostLoad: { hosts: {}, unassignedWaiting: 0 } })?.executionHostLoad).toEqual({ hosts: {}, unassignedWaiting: 0 })
    expect(sanitizeCppStatePatch({ statusLine: '' })?.executionHostLoad).toBeUndefined()
  })
})
