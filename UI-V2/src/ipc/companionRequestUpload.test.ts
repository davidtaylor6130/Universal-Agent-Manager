import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { COMPANION_DIRECT_REQUEST_BYTES, sendToCEF } from './cefBridge'

const WIRE_LIMIT = 65536
const CHUNK_BYTES = 32768

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

type Reply = { ok: boolean; status: number; json: () => Promise<unknown> }
const reply = (body: unknown, status = 200): Reply => ({ ok: status >= 200 && status < 300, status, json: async () => body })

/** Mirrors the companion listener: a 64 KiB wire cap, exact-offset appends and a single-use commit. */
function mockCompanionServer(options: { requestChunks?: boolean; failAppendAt?: number } = {}) {
  const uploads = new Map<string, { total: number; bytes: number[] }>()
  const dispatched: Array<{ action: string; payload: unknown }> = []
  const fetch = vi.fn(async (_url: string, init: RequestInit) => {
    const body = String(init.body)
    if (new TextEncoder().encode(body).length > WIRE_LIMIT) return reply({ error: 'Request exceeds 64 KiB.' }, 413)
    const request = JSON.parse(body)
    const dispatch = (envelope: { action: string; payload: unknown }) => {
      dispatched.push(envelope)
      return reply({ accepted: envelope.action })
    }
    switch (request.action) {
      case 'getCompanionCapabilities':
        return reply({ protocolVersion: 1, features: { requestChunks: options.requestChunks ?? true }, limits: { maxRequestBytes: 32 * 1024 * 1024, requestChunkBytes: CHUNK_BYTES } })
      case 'beginCompanionRequestTransfer': {
        const id = `upload-${uploads.size + 1}`
        uploads.set(id, { total: request.payload.totalBytes, bytes: [] })
        return reply({ id, totalBytes: request.payload.totalBytes, chunkBytes: CHUNK_BYTES, nextOffset: 0, expiresInSeconds: 120 })
      }
      case 'appendCompanionRequestChunk': {
        const upload = uploads.get(request.payload.id)
        if (!upload || request.payload.offset !== upload.bytes.length) return reply({ error: 'Offset mismatch.' }, 409)
        if (request.payload.offset === options.failAppendAt) return reply({ error: 'Upload capacity exhausted.' }, 503)
        const decoded = Array.from(atob(request.payload.base64), (character) => character.charCodeAt(0))
        if (decoded.length > CHUNK_BYTES) return reply({ error: 'Chunk too large.' }, 413)
        upload.bytes.push(...decoded)
        return reply({ id: request.payload.id, nextOffset: upload.bytes.length })
      }
      case 'commitCompanionRequestTransfer': {
        const upload = uploads.get(request.payload.id)
        uploads.delete(request.payload.id)
        if (!upload || upload.bytes.length !== upload.total) return reply({ error: 'Transfer not found.' }, 404)
        return dispatch(JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array(upload.bytes))))
      }
      default:
        return dispatch(request)
    }
  })
  vi.stubGlobal('fetch', fetch)
  const actions = () => fetch.mock.calls.map((call) => JSON.parse(String(call[1].body)).action as string)
  return { fetch, dispatched, actions }
}

/** Builds a prompt whose serialized envelope is exactly `bytes` long, with multi-byte characters across chunk edges. */
function promptOfEnvelopeBytes(bytes: number, requestId: string) {
  const envelopeBytes = (text: string) => new TextEncoder().encode(JSON.stringify({ action: 'sendAcpPrompt', payload: { chatId: 'chat-1', text }, requestId })).length
  const base = '🙂 long prompt '.repeat(2000)
  const text = base + 'x'.repeat(bytes - envelopeBytes(base))
  expect(envelopeBytes(text)).toBe(bytes)
  return { action: 'sendAcpPrompt', payload: { chatId: 'chat-1', text }, requestId }
}

beforeEach(() => {
  Object.defineProperty(window, 'localStorage', { configurable: true, value: createMemoryStorage() })
  window.history.replaceState(null, '', '/companion')
  window.localStorage.setItem('uam-companion-token', 'test-token')
})

afterEach(() => {
  vi.unstubAllGlobals()
  window.localStorage.clear()
  window.history.replaceState(null, '', '/')
})

it('sends envelopes just under the direct limit as one ordinary request', async () => {
  const server = mockCompanionServer()
  const request = promptOfEnvelopeBytes(COMPANION_DIRECT_REQUEST_BYTES - 1, 'direct')
  expect(await sendToCEF(request)).toMatchObject({ ok: true, data: { accepted: 'sendAcpPrompt' } })
  expect(server.actions()).toEqual(['sendAcpPrompt'])
})

it('uploads a request of 61,440 bytes or more in chunks and commits it exactly once', async () => {
  const server = mockCompanionServer()
  for (const size of [COMPANION_DIRECT_REQUEST_BYTES, 200 * 1024]) {
    server.fetch.mockClear()
    server.dispatched.length = 0
    const request = promptOfEnvelopeBytes(size, `upload-${size}`)
    const result = await sendToCEF(request)

    expect(result).toMatchObject({ ok: true, data: { accepted: 'sendAcpPrompt' }, requestId: request.requestId })
    const chunks = Math.ceil(size / CHUNK_BYTES)
    expect(server.actions()).toEqual(['getCompanionCapabilities', 'beginCompanionRequestTransfer', ...Array(chunks).fill('appendCompanionRequestChunk'), 'commitCompanionRequestTransfer'])
    expect(server.fetch.mock.calls.every((call) => new TextEncoder().encode(String(call[1].body)).length <= WIRE_LIMIT)).toBe(true)
    expect(server.fetch.mock.calls.every((call) => (call[1].headers as Record<string, string>).Authorization === 'Bearer test-token')).toBe(true)
    // The server receives the complete original envelope, not just its payload.
    expect(server.dispatched).toEqual([request])
  }
})

it('stops before commit when an append fails, reports the request as not sent and never retries', async () => {
  const server = mockCompanionServer({ failAppendAt: CHUNK_BYTES })
  const result = await sendToCEF(promptOfEnvelopeBytes(100 * 1024, 'append-fails'), false)
  expect(result).toMatchObject({ ok: false, error: 'Upload capacity exhausted. The request was not sent.' })
  expect(server.actions()).toEqual(['getCompanionCapabilities', 'beginCompanionRequestTransfer', 'appendCompanionRequestChunk', 'appendCompanionRequestChunk'])
  expect(server.dispatched).toEqual([])
})

it('does not begin an upload when UAM does not offer request chunks', async () => {
  const server = mockCompanionServer({ requestChunks: false })
  const result = await sendToCEF(promptOfEnvelopeBytes(70 * 1024, 'no-chunks'), false)
  expect(result.ok).toBe(false)
  expect(result.error).toContain('too large to send without chunked uploads')
  expect(server.actions()).toEqual(['getCompanionCapabilities'])
})

it('treats a lost commit reply as an unknown outcome without committing again', async () => {
  const server = mockCompanionServer()
  const respond = server.fetch.getMockImplementation()!
  server.fetch.mockImplementation(async (url: string, init: RequestInit) => {
    if (JSON.parse(String(init.body)).action === 'commitCompanionRequestTransfer') {
      await respond(url, init)
      throw new TypeError('Failed to fetch')
    }
    return respond(url, init)
  })
  const result = await sendToCEF(promptOfEnvelopeBytes(64 * 1024, 'lost-commit'), false)
  expect(result).toMatchObject({ ok: false, error: 'Connection lost. Check the chat before retrying; the action may have reached UAM.' })
  expect(server.actions().filter((action) => action === 'commitCompanionRequestTransfer')).toHaveLength(1)
  expect(server.actions().filter((action) => action === 'beginCompanionRequestTransfer')).toHaveLength(1)
  expect(server.dispatched).toHaveLength(1)
})
