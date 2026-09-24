import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { sendToCEF } from './cefBridge'

const createMemoryStorage = (): Storage => {
  const entries = new Map<string, string>()
  return {
    get length() { return entries.size },
    clear: () => entries.clear(),
    getItem: (key) => entries.get(key) ?? null,
    key: (index) => Array.from(entries.keys())[index] ?? null,
    removeItem: (key) => { entries.delete(key) },
    setItem: (key, value) => { entries.set(key, String(value)) },
  }
}

beforeEach(() => {
  Object.defineProperty(window, 'localStorage', { configurable: true, value: createMemoryStorage() })
})

afterEach(() => {
  vi.unstubAllGlobals()
  window.localStorage.clear()
  window.history.replaceState(null, '', '/')
})

it('authenticates companion requests, preserves failures and keeps selection local without retrying', async () => {
  window.history.replaceState(null, '', '/companion')
  const fetch = vi.fn()
  vi.stubGlobal('fetch', fetch)
  expect((await sendToCEF({ action: 'getInitialState' })).ok).toBe(false)
  expect((await sendToCEF({ action: 'selectSession' })).ok).toBe(true)
  expect(fetch).not.toHaveBeenCalled()
  window.localStorage.setItem('uam-companion-token', 'test-token')
  fetch.mockResolvedValueOnce({ ok: true, json: async () => ({ chats: [] }) })
  expect((await sendToCEF({ action: 'getInitialState' })).data).toEqual({ chats: [] })
  expect(fetch.mock.calls[0][1].headers.Authorization).toBe('Bearer test-token')
  fetch.mockResolvedValueOnce({ ok: true, json: async () => ({ ok: false, error: 'Rejected' }) })
  expect((await sendToCEF({ action: 'sendAcpPrompt' })).error).toBe('Rejected')
  fetch.mockRejectedValueOnce(new Error('offline'))
  expect((await sendToCEF({ action: 'sendAcpPrompt' })).ok).toBe(false)
  expect(fetch).toHaveBeenCalledTimes(3)
})

it('reassembles response chunks across UTF-8 boundaries without replaying the original action', async () => {
  window.history.replaceState(null, '', '/companion')
  window.localStorage.setItem('uam-companion-token', 'test-token')
  const expected = { messages: [{ content: 'A large response 🙂 ends here' }] }
  const bytes = new TextEncoder().encode(JSON.stringify(expected))
  const split = bytes.indexOf(0xf0) + 2
  const encode = (part: Uint8Array) => {
    let binary = ''
    for (let offset = 0; offset < part.length; offset += 0x8000) binary += String.fromCharCode(...part.subarray(offset, offset + 0x8000))
    return btoa(binary)
  }
  const fetch = vi.fn()
    .mockResolvedValueOnce({ ok: true, json: async () => ({ uamTransfer: { id: 'transfer', totalBytes: bytes.length, chunkBytes: split } }) })
    .mockResolvedValueOnce({ ok: true, json: async () => ({ base64: encode(bytes.slice(0, split)), nextOffset: split, done: false }) })
    .mockResolvedValueOnce({ ok: true, json: async () => ({ base64: encode(bytes.slice(split)), nextOffset: bytes.length, done: true }) })
  vi.stubGlobal('fetch', fetch)
  expect((await sendToCEF({ action: 'getChatMessages' })).data).toEqual(expected)
  expect(fetch.mock.calls.map((call) => JSON.parse(call[1].body).action)).toEqual([
    'getChatMessages', 'getCompanionResponseChunk', 'getCompanionResponseChunk',
  ])
  expect(JSON.parse(fetch.mock.calls[2][1].body).payload.offset).toBe(split)
})

it('rejects broken chunks without returning a partial chat or retrying the original request', async () => {
  window.history.replaceState(null, '', '/companion')
  window.localStorage.setItem('uam-companion-token', 'test-token')
  const fetch = vi.fn()
    .mockResolvedValueOnce({ ok: true, json: async () => ({ uamTransfer: { id: 'transfer', totalBytes: 100 } }) })
    .mockResolvedValueOnce({ ok: true, json: async () => ({ base64: btoa('{}'), nextOffset: 99, done: false }) })
  vi.stubGlobal('fetch', fetch)
  const result = await sendToCEF({ action: 'getChatMessages' })
  expect(result.ok).toBe(false)
  expect(result.data).toBeUndefined()
  expect(fetch).toHaveBeenCalledTimes(2)
})

it('loads up to four chunks concurrently, accepts out-of-order replies and waits before the deleting final chunk', async () => {
  window.history.replaceState(null, '', '/companion')
  window.localStorage.setItem('uam-companion-token', 'test-token')
  const chunkBytes = 128 * 1024
  const expected = { messages: [{ content: 'x'.repeat(chunkBytes * 5) }] }
  const bytes = new TextEncoder().encode(JSON.stringify(expected))
  const encode = (part: Uint8Array) => {
    let binary = ''
    for (let offset = 0; offset < part.length; offset += 0x8000) binary += String.fromCharCode(...part.subarray(offset, offset + 0x8000))
    return btoa(binary)
  }
  const pending = new Map<number, (response: unknown) => void>()
  const fetch = vi.fn(async (_url: string, init: RequestInit) => {
    const request = JSON.parse(String(init.body))
    if (request.action === 'getChatMessages') return { ok: true, json: async () => ({ uamTransfer: { id: 'transfer', totalBytes: bytes.length, chunkBytes } }) }
    const offset = request.payload.offset
    return await new Promise((resolve) => pending.set(offset, resolve)) as { ok: boolean; json: () => Promise<unknown> }
  })
  vi.stubGlobal('fetch', fetch)
  const resultPromise = sendToCEF({ action: 'getChatMessages' })
  await vi.waitFor(() => expect(pending.size).toBe(4))
  expect([...pending.keys()]).toEqual([0, chunkBytes, chunkBytes * 2, chunkBytes * 3])
  for (const offset of [...pending.keys()].reverse()) {
    pending.get(offset)!({ ok: true, json: async () => ({ base64: encode(bytes.slice(offset, offset + chunkBytes)), nextOffset: Math.min(offset + chunkBytes, bytes.length), done: false }) })
    pending.delete(offset)
  }
  await vi.waitFor(() => expect(pending.size).toBe(1))
  expect([...pending.keys()]).toEqual([chunkBytes * 4])
  const penultimate = chunkBytes * 4
  pending.get(penultimate)!({ ok: true, json: async () => ({ base64: encode(bytes.slice(penultimate, penultimate + chunkBytes)), nextOffset: Math.min(penultimate + chunkBytes, bytes.length), done: false }) })
  pending.delete(penultimate)
  await vi.waitFor(() => expect(pending.size).toBe(1))
  const finalOffset = Math.floor((bytes.length - 1) / chunkBytes) * chunkBytes
  expect([...pending.keys()]).toEqual([finalOffset])
  pending.get(finalOffset)!({ ok: true, json: async () => ({ base64: encode(bytes.slice(finalOffset)), nextOffset: bytes.length, done: true }) })
  expect((await resultPromise).data).toEqual(expected)
})

it('fails a concurrent transfer when any chunk request fails and does not fetch the final chunk', async () => {
  window.history.replaceState(null, '', '/companion')
  window.localStorage.setItem('uam-companion-token', 'test-token')
  const chunkBytes = 128 * 1024
  const bytes = new TextEncoder().encode(JSON.stringify({ content: 'x'.repeat(chunkBytes * 4) }))
  const encode = (part: Uint8Array) => {
    let binary = ''
    for (let offset = 0; offset < part.length; offset += 0x8000) binary += String.fromCharCode(...part.subarray(offset, offset + 0x8000))
    return btoa(binary)
  }
  const fetch = vi.fn(async (_url: string, init: RequestInit) => {
    const request = JSON.parse(String(init.body))
    if (request.action === 'getChatMessages') return { ok: true, json: async () => ({ uamTransfer: { id: 'transfer', totalBytes: bytes.length, chunkBytes } }) }
    if (request.payload.offset === chunkBytes) return { ok: false, json: async () => ({ error: 'Transfer expired.' }) }
    return { ok: true, json: async () => ({ base64: encode(bytes.slice(request.payload.offset, request.payload.offset + chunkBytes)), nextOffset: Math.min(request.payload.offset + chunkBytes, bytes.length), done: false }) }
  })
  vi.stubGlobal('fetch', fetch)
  const result = await sendToCEF({ action: 'getChatMessages' })
  expect(result.ok).toBe(false)
  expect(result.error).toBe('Transfer expired.')
  expect(result.data).toBeUndefined()
  expect(fetch.mock.calls.filter((call) => JSON.parse(String(call[1].body)).action === 'getChatMessages')).toHaveLength(1)
  expect(fetch.mock.calls.some((call) => JSON.parse(String(call[1].body)).payload?.offset === Math.floor((bytes.length - 1) / chunkBytes) * chunkBytes)).toBe(false)
})
