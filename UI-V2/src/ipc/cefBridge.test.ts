import { afterEach, describe, expect, it, vi } from 'vitest'
import { boundedRead } from './cefBridge'

describe('boundedRead', () => {
  afterEach(() => { vi.useRealTimers() })

  it('keeps a rejected reply’s own error instead of the timeout message', async () => {
    await expect(boundedRead(Promise.reject(new Error('Bridge closed.')), 'Did not answer in time.')).resolves.toEqual({ ok: false, error: 'Bridge closed.' })
    await expect(boundedRead(Promise.reject('Native refused.'), 'Did not answer in time.')).resolves.toEqual({ ok: false, error: 'Native refused.' })
    await expect(boundedRead(Promise.reject(undefined), 'Did not answer in time.')).resolves.toEqual({ ok: false })
  })

  it('reports the timeout message only when the deadline passes', async () => {
    vi.useFakeTimers()
    const read = boundedRead(new Promise(() => {}), 'Did not answer in time.', 1_000)
    await vi.advanceTimersByTimeAsync(1_000)
    await expect(read).resolves.toEqual({ ok: false, error: 'Did not answer in time.' })
  })

  it('returns the reply when it answers first', async () => {
    await expect(boundedRead(Promise.resolve({ ok: true, data: 1 }), 'Did not answer in time.')).resolves.toEqual({ ok: true, data: 1 })
  })
})
