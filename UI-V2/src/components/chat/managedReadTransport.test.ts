import { afterEach, describe, expect, it, vi } from 'vitest'

type Sent = { request: { action: string; payload: { chatId: string } }; settled: boolean; promise: Promise<unknown>; resolve: (value: unknown) => void }
const transport = vi.hoisted(() => ({ sent: [] as Sent[] }))
vi.mock('../../ipc/cefBridge', () => ({
  isCefContext: () => true,
  sendToCEF: vi.fn((request: Sent['request']) => {
    let resolve!: (value: unknown) => void
    const promise = new Promise(done => { resolve = done })
    const row: Sent = { request, settled: false, promise, resolve: value => { row.settled = true; resolve(value) } }
    transport.sent.push(row)
    return promise
  }),
}))
import { cancelManagedSubagent, listManagedSubagents } from './managedSubagents'

const emptyPage = (chatId: string) => ({ ok: true, data: { chatId, controlScopeId: 'scope', runs: [], offset: 0, nextOffset: null, totalCount: 0 } })
const unacknowledged = () => transport.sent.filter(row => !row.settled).length
const chats = () => transport.sent.map(row => row.request.payload.chatId)
const flush = () => act(0)
async function act(_: number) { for (let tick = 0; tick < 12; ++tick) await Promise.resolve() }
const settle = (row: Sent) => row.resolve(emptyPage(row.request.payload.chatId))
const read = (chat: string, signal: AbortSignal) => listManagedSubagents(chat, '', 0, 20, signal).then(value => ({ value }), (error: Error) => ({ error }))

// Module-global slots are owned by bridge calls, so every test acknowledges everything it sent.
afterEach(async () => {
  vi.useRealTimers()
  for (let round = 0; round < 6 && unacknowledged(); ++round) { transport.sent.filter(row => !row.settled).forEach(settle); await flush() }
  expect(unacknowledged()).toBe(0)
  transport.sent.length = 0
})

describe('managed read transport bound', () => {
  it('keeps a slot until the bridge call settles, even after UI aborts release both logical callers', async () => {
    const controllers = [new AbortController(), new AbortController(), new AbortController()]
    const first = read('p0', controllers[0].signal), second = read('p1', controllers[1].signal)
    await flush()
    expect(chats()).toEqual(['p0', 'p1'])
    controllers[0].abort(); controllers[1].abort()
    expect((await Promise.all([first, second])).map(result => (result as { error: Error }).error.name)).toEqual(['AbortError', 'AbortError'])
    const third = read('p2', controllers[2].signal)
    await flush()
    expect(chats(), 'no third bridge call while two aborted calls are still unacknowledged').toEqual(['p0', 'p1'])
    expect(unacknowledged()).toBe(2)
    settle(transport.sent[0])
    await flush()
    expect(chats()).toEqual(['p0', 'p1', 'p2'])
    expect(unacknowledged()).toBe(2)
    settle(transport.sent[2]); await third
  })

  it('also keeps the slot after the 30s semantic timeout rejects the caller', async () => {
    vi.useFakeTimers()
    const controller = new AbortController()
    const timedOut = read('slow-0', controller.signal), slow = read('slow-1', controller.signal)
    const queued = read('queued', controller.signal)
    await vi.advanceTimersByTimeAsync(30000)
    expect((await timedOut as { error: Error }).error.message).toMatch(/timed out/)
    expect((await slow as { error: Error }).error.message).toMatch(/timed out/)
    expect((await queued as { error: Error }).error.message).toMatch(/timed out/)
    expect(chats(), 'the queued read left the queue instead of sending').toEqual(['slow-0', 'slow-1'])
    const next = read('next', new AbortController().signal)
    await vi.advanceTimersByTimeAsync(0)
    expect(chats()).toEqual(['slow-0', 'slow-1'])
    settle(transport.sent[0]); await vi.advanceTimersByTimeAsync(0)
    expect(chats()).toEqual(['slow-0', 'slow-1', 'next'])
    settle(transport.sent[2]); await next
  })

  it('hands released slots to waiters in order and a late caller cannot overtake them', async () => {
    const live = new AbortController()
    const results = ['a', 'b', 'c', 'd'].map(id => read(id, live.signal))
    await flush()
    expect(chats()).toEqual(['a', 'b'])
    settle(transport.sent[0])
    const late = read('late', live.signal) // arrives between the release and the woken waiter's continuation
    await flush()
    expect(chats()).toEqual(['a', 'b', 'c'])
    settle(transport.sent[1]); await flush()
    expect(chats()).toEqual(['a', 'b', 'c', 'd'])
    settle(transport.sent[2]); await flush()
    expect(chats()).toEqual(['a', 'b', 'c', 'd', 'late'])
    transport.sent.filter(row => !row.settled).forEach(settle)
    await Promise.all([...results, late])
  })

  it('removes a cancelled queued waiter and never sends it', async () => {
    const live = new AbortController(), cancelled = new AbortController()
    const results = [read('a', live.signal), read('b', live.signal)]
    const dropped = read('dropped', cancelled.signal)
    results.push(read('after', live.signal))
    await flush()
    cancelled.abort()
    expect((await dropped as { error: Error }).error.name).toBe('AbortError')
    settle(transport.sent[0]); await flush()
    expect(chats()).toEqual(['a', 'b', 'after'])
    transport.sent.filter(row => !row.settled).forEach(settle)
    await Promise.all(results)
  })

  it('does not send for a waiter aborted after it was woken, and passes the slot on', async () => {
    const live = new AbortController(), woken = new AbortController()
    const results = [read('a', live.signal), read('b', live.signal)]
    const abortedAfterWake = read('woken', woken.signal)
    results.push(read('next', live.signal))
    await flush()
    // Registered after the limiter's own release callback, so it runs once the waiter has been granted the slot.
    void transport.sent[0].promise.then(() => woken.abort())
    settle(transport.sent[0]); await flush()
    expect((await abortedAfterWake as { error: Error }).error.name).toBe('AbortError')
    expect(chats()).toEqual(['a', 'b', 'next'])
    transport.sent.filter(row => !row.settled).forEach(settle)
    await Promise.all(results)
  })

  it('does not queue Stop behind hung reads', async () => {
    const live = new AbortController()
    const reads = [read('a', live.signal), read('b', live.signal), read('c', live.signal)]
    await flush()
    const stop = cancelManagedSubagent('parent', 'run-1', 'scope-1')
    await flush()
    expect(transport.sent.map(row => row.request.action)).toEqual(['listManagedSubagents', 'listManagedSubagents', 'cancelManagedSubagent'])
    transport.sent[2].resolve({ ok: true, data: { runId: 'run-1' } }); await stop
    transport.sent.filter(row => !row.settled).forEach(settle)
    await flush()
    transport.sent.filter(row => !row.settled).forEach(settle)
    await Promise.all(reads)
  })
})


it('does not send a just-woken waiter when its unchanged30s deadline fires before continuation', async () => {
  vi.useFakeTimers()
  const live = new AbortController()
  const results = [read('deadline-a', live.signal), read('deadline-b', live.signal)]
  const woken = read('deadline-woken', live.signal)
  results.push(read('deadline-next', live.signal))
  await flush()
  expect(unacknowledged()).toBe(2)
  void transport.sent[0].promise.then(() => vi.advanceTimersByTime(30000))
  settle(transport.sent[0]); await flush()
  expect((await woken as { error: Error }).error.name).toBe('AbortError')
  expect(chats()).toEqual(['deadline-a', 'deadline-b'])
  expect(unacknowledged()).toBeLessThanOrEqual(2)
  transport.sent.filter(row => !row.settled).forEach(settle)
  await Promise.all(results)
})
