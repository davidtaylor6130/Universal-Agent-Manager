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
  const encode = (part: Uint8Array) => btoa(String.fromCharCode(...part))
  const fetch = vi.fn()
    .mockResolvedValueOnce({ ok: true, json: async () => ({ uamTransfer: { id: 'transfer', totalBytes: bytes.length } }) })
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
