import { describe, expect, it } from 'vitest'
import { act, createElement } from 'react'
import { createRoot } from 'react-dom/client'
import { useUsageHistory } from '../hooks/useUsageHistory'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
import { assembleUsageHistory, EMPTY_USAGE_TOTALS, parseUsageHistory, USAGE_PROVIDERS, type UsageHistory } from './usageHistory'

const encoder = new TextEncoder()
function pages(value: unknown) {
  const body = JSON.stringify(value)
  const result: Record<string, unknown>[] = []
  let offset = 0
  // Iterate code points so the test transport also preserves surrogate pairs.
  let chunk = '', characters = 0
  const totalBytes = encoder.encode(body).length
  for (const character of body) {
    if (characters === 32768) {
      const bytes = encoder.encode(chunk).length
      result.push({ usagePage: 1, snapshotId: 'snapshot', refreshId: 'request', offset, nextOffset: offset + bytes, totalBytes, chunk })
      offset += bytes
      chunk = ''; characters = 0
    }
    chunk += character; characters++
  }
  result.push({ usagePage: 1, snapshotId: 'snapshot', refreshId: 'request', offset, nextOffset: null, totalBytes, chunk })
  return result
}
function summary(modelCount: number): UsageHistory {
  return { version: 2, source: 'native-transcripts', executionHostId: 'local', startMs: 86400000, endMs: 172800000, totals: { ...EMPTY_USAGE_TOTALS }, complete: true,
    filesChecked: 0, filesTotal: 0, bytesRead: 0, duplicates: 0, unidentifiedRecords: 0, listingLimited: false,
    providers: USAGE_PROVIDERS.map(providerId => ({ ...EMPTY_USAGE_TOTALS, providerId, available: true, files: 0, errors: 0, invalidLines: 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 })),
    models: Array.from({ length: modelCount }, (_, index) => ({ ...EMPTY_USAGE_TOTALS, providerId: 'codex-cli' as const, modelId: `${index}模型🦄`, speed: 'standard' as const })),
    days: [], hours: [], providerDays: [], providerHours: [] }
}

describe('usage snapshot transport', () => {
  it('reassembles immutable UTF8 pages before exposing totals and preserves more than 20000 models', async () => {
    const original = summary(20001)
    const body = JSON.stringify(original)
    const chunks = pages(original)
    let index = 1
    const assembled = await assembleUsageHistory(chunks[0], 'request', async (id, offset) => {
      expect(id).toBe('snapshot')
      expect(offset).toBe(chunks[index].offset)
      return chunks[index++]
    }, () => false)
    expect(assembled).toEqual(original)
    expect(parseUsageHistory(assembled, 'local', { startMs: 86400000, endMs: 172800000, days: 1 }).models).toHaveLength(20001)
    expect(encoder.encode(body).length).toBeGreaterThan(2 * 1024 * 1024)
  })

  it('rejects changed ownership, reordered pages, truncated final data and obsolete loads', async () => {
    const chunks = pages({ value: '模型🦄'.repeat(40000) })
    for (const changes of [{ snapshotId: 'other' }, { refreshId: 'other' }, { offset: 0 }, { totalBytes: 1 }, { nextOffset: null }, { chunk: '' }]) {
      await expect(assembleUsageHistory(chunks[0], 'request', async () => ({ ...chunks[1], ...changes }), () => false)).rejects.toThrow('Usage snapshot')
    }
    await expect(assembleUsageHistory(chunks[0], 'request', async () => chunks[1], () => true)).rejects.toThrow('cancelled')
  })

  it('preserves small replies and refuses byte oversized or stalled pages', async () => {
    const small = summary(1)
    expect(await assembleUsageHistory(small, 'request', async () => { throw new Error('unexpected continuation') }, () => false)).toBe(small)
    const page = { usagePage: 1, snapshotId: 'snapshot', refreshId: 'request', offset: 0, nextOffset: null, totalBytes: 300000, chunk: 'x'.repeat(300000) }
    await expect(assembleUsageHistory(page, 'request', async () => null, () => false)).rejects.toThrow('page')
  })

  it('keeps partial pages private and binds hook continuations to the original request', async () => {
    const original = summary(1000), chunks = pages(original)
    const range = { startMs: 86400000, endMs: 172800000, days: 1 as const }
    let state!: ReturnType<typeof useUsageHistory>
    let originalPayload: Record<string, unknown> = {}, held: (() => void) | undefined
    let fail = false, hold = true
    window.cefQuery = request => {
      const payload = JSON.parse(request.request).payload as Record<string, unknown>
      if (!payload.snapshotId) originalPayload = payload
      else expect(payload).toEqual({ ...originalPayload, snapshotId: 'snapshot', offset: payload.offset })
      const page = chunks.find(row => row.offset === (payload.offset ?? 0))!
      const respond = () => {
        if (fail && payload.snapshotId) request.onFailure(409, 'Usage snapshot expired or changed. Refresh to try again.')
        else request.onSuccess(JSON.stringify({ ...page, refreshId: payload.refreshId }))
      }
      if (payload.snapshotId && hold) { hold = false; held = respond } else respond()
    }
    function Probe() { state = useUsageHistory('local', range); return null }
    const root = createRoot(document.createElement('div'))
    try {
      await act(async () => root.render(createElement(Probe)))
      expect(state.data).toBeNull()
      expect(state.loading).toBe(true)
      expect(held).toBeDefined()
      await act(async () => held!())
      expect(state.data?.models).toHaveLength(1000)
      const lastGood = state.data
      fail = true
      await act(async () => state.refresh())
      expect(state.data).toBe(lastGood)
      expect(state.error).toContain('expired or changed')
    } finally {
      act(() => root.unmount())
      delete window.cefQuery
    }
  })

})
