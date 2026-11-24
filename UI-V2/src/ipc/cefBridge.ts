import { companionPlatformAction } from './companionPlatform'
import { MOBILE_COMPANION_ENABLED } from '../config/buildFeatures'

/**
 * Routes requests through the C++ CefMessageRouter when hosted in CEF.
 * Standalone browser previews receive mock responses.
 */

declare global {
  interface Window {
    cefQuery?: (params: {
      request: string
      onSuccess: (response: string) => void
      onFailure: (errorCode: number, errorMessage: string) => void
    }) => void
    /** C++ → JS push channel. Payload can be JSON string or object. */
    uamPush?: (payload: unknown) => void
  }
}

/** Identifies the companion route sharing the desktop UI and store. */
export function isCompanionContext(): boolean {
  return MOBILE_COMPANION_ENABLED && typeof window !== 'undefined' && window.location.pathname.replace(/\/$/, '') === '/companion'
}

export function isCefContext(): boolean {
  return isCompanionContext() || typeof window !== 'undefined' && typeof window.cefQuery === 'function'
}

export interface CEFRequest {
  action: string
  payload?: unknown
  requestId?: string
}

export interface CEFResponse<T = unknown> {
  ok: boolean
  data?: T
  error?: string
  /** Native CEF failure code; absent for companion and transport failures. */
  code?: number
  /** A native success body that finished with a caveat, such as a helper installed without confirmed startup. */
  warning?: string
  requestId?: string
}

export function createRequestId(prefix = 'cef'): string {
  const cryptoObj = typeof globalThis !== 'undefined' ? globalThis.crypto : undefined
  if (cryptoObj && typeof cryptoObj.randomUUID === 'function') {
    return cryptoObj.randomUUID()
  }

  return `${prefix}-${Date.now()}-${Math.random().toString(16).slice(2)}`
}

/** A typed secret never reaches a log; every other field stays readable. */
export function redactedEnvelope(envelope: CEFRequest): CEFRequest {
  const payload = envelope.payload
  if (envelope.action !== 'submitSecretRequest' || typeof payload !== 'object' || payload === null || !('value' in payload)) return envelope
  return { ...envelope, payload: { ...payload, value: '[redacted]' } }
}

/** The companion listener rejects wire bodies over 64 KiB; envelopes of 60 KiB or more are uploaded in chunks instead. */
export const COMPANION_DIRECT_REQUEST_BYTES = 60 * 1024
const COMPANION_MAX_REQUEST_BYTES = 32 * 1024 * 1024
const COMPANION_MAX_REQUEST_CHUNK_BYTES = 32 * 1024
/** Matches the iPhone client: a server advertising tiny chunks cannot drive an unbounded number of appends. */
const COMPANION_MAX_REQUEST_CHUNKS = 4096
const COMPANION_TRANSFER_ACTIONS = ['beginCompanionRequestTransfer', 'appendCompanionRequestChunk', 'commitCompanionRequestTransfer']
const COMPANION_TOKEN_REJECTED = 'UAM no longer accepts this connection token. Log out in Settings, then connect with the current token from the computer.'
const COMPANION_UPLOAD_UNAVAILABLE = 'This request is too large to send without chunked uploads. Update UAM on the computer or shorten the request.'
const COMPANION_REQUEST_TOO_LARGE = 'This request is too large to send to UAM. Shorten it or send fewer attachments.'

const companionPost = (token: string, body: string) => fetch('/api', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${token}` },
  body,
  signal: AbortSignal.timeout(30000),
  redirect: 'error',
  cache: 'no-store',
})

const companionFailure = async (response: Response) => response.status === 401
  // A rotated token (new pairing code or Regenerate token on the computer) ends this browser's pairing.
  ? COMPANION_TOKEN_REJECTED
  : (await response.json().catch(() => null))?.error ?? `Request failed (${response.status}).`

const toBase64 = (bytes: Uint8Array) => {
  let binary = ''
  for (let offset = 0; offset < bytes.length; offset += 0x2000) binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x2000))
  return btoa(binary)
}

/**
 * Uploads a whole request envelope with begin, append and one commit, as the protocol requires above the direct limit.
 * Every failure before commit means UAM never received the action. The commit is sent exactly once and never
 * repeated: a lost commit reply leaves the outcome unknown (docs/reference/companion-protocol.md).
 */
async function uploadCompanionRequest(token: string, action: string, bytes: Uint8Array): Promise<{ response: Response } | { error: string }> {
  const notSent = (reason: string) => ({ error: `${reason} The request was not sent.` })
  if (COMPANION_TRANSFER_ACTIONS.includes(action)) return notSent('Nested request uploads are unsupported.')
  if (bytes.length > COMPANION_MAX_REQUEST_BYTES) return { error: COMPANION_REQUEST_TOO_LARGE }
  const step = async <T>(stepAction: string, payload: Record<string, unknown>): Promise<{ data: T } | { error: string }> => {
    if (window.localStorage.getItem('uam-companion-token') !== token) return notSent('Disconnected.')
    let response: Response
    try { response = await companionPost(token, JSON.stringify({ action: stepAction, payload })) } catch { return notSent('Connection lost while uploading.') }
    if (!response.ok) {
      if (response.status === 401) return { error: COMPANION_TOKEN_REJECTED }
      // An older UAM without request uploads does not list or route the capabilities action.
      if ((response.status === 403 || response.status === 404) && stepAction === 'getCompanionCapabilities') return { error: COMPANION_UPLOAD_UNAVAILABLE }
      return notSent(await companionFailure(response))
    }
    const data = await response.json().catch(() => null)
    return data && typeof data === 'object' ? { data: data as T } : notSent('UAM returned an invalid upload reply.')
  }
  const capabilities = await step<{ protocolVersion?: unknown; features?: { requestChunks?: unknown }; limits?: { maxRequestBytes?: unknown; requestChunkBytes?: unknown } }>('getCompanionCapabilities', {})
  if ('error' in capabilities) return capabilities
  const { maxRequestBytes, requestChunkBytes: chunkBytes } = capabilities.data.limits ?? {}
  if (capabilities.data.protocolVersion !== 1 || capabilities.data.features?.requestChunks !== true
    || !Number.isSafeInteger(maxRequestBytes) || (maxRequestBytes as number) <= 0
    || !Number.isSafeInteger(chunkBytes) || (chunkBytes as number) <= 0 || (chunkBytes as number) > COMPANION_MAX_REQUEST_CHUNK_BYTES
    || Math.ceil(bytes.length / (chunkBytes as number)) > COMPANION_MAX_REQUEST_CHUNKS) return { error: COMPANION_UPLOAD_UNAVAILABLE }
  if (bytes.length > (maxRequestBytes as number)) return { error: COMPANION_REQUEST_TOO_LARGE }
  const transfer = await step<{ id?: unknown; totalBytes?: unknown; chunkBytes?: unknown; nextOffset?: unknown }>('beginCompanionRequestTransfer', { totalBytes: bytes.length })
  if ('error' in transfer) return transfer
  const { id } = transfer.data
  if (typeof id !== 'string' || id.length === 0 || id.length > 128 || transfer.data.totalBytes !== bytes.length
    || transfer.data.nextOffset !== 0 || transfer.data.chunkBytes !== chunkBytes) return notSent('UAM returned an invalid upload descriptor.')
  for (let offset = 0; offset < bytes.length;) {
    const next = Math.min(offset + (chunkBytes as number), bytes.length)
    const acknowledgement: { data: { id?: unknown; nextOffset?: unknown } } | { error: string } = await step('appendCompanionRequestChunk', { id, offset, base64: toBase64(bytes.subarray(offset, next)) })
    if ('error' in acknowledgement) return acknowledgement
    if (acknowledgement.data.id !== id || acknowledgement.data.nextOffset !== next) return notSent('UAM returned an invalid upload acknowledgement.')
    offset = next
  }
  if (window.localStorage.getItem('uam-companion-token') !== token) return notSent('Disconnected.')
  return { response: await companionPost(token, JSON.stringify({ action: 'commitCompanionRequestTransfer', payload: { id } })) }
}

/**
 * Send a request to the C++ backend.
 * In dev: returns a mock response after a short delay.
 * In production (CEF): delegates to window.cefQuery().
 */
export async function sendToCEF<T = unknown>(
  request: CEFRequest,
  logFailures = true,
): Promise<CEFResponse<T>> {
  const requestId = request.requestId ?? createRequestId()
  const envelope: CEFRequest = { ...request, requestId }

  if (isCompanionContext()) {
    // Selection belongs to this client; never move the desktop's current chat.
    if (['selectSession', 'toggleFolder', 'toggleResourceCollection'].includes(request.action)) return { ok: true, data: {} as T, requestId }
    const local = await companionPlatformAction(request.action, request.payload)
    if (local) return { ...local, data: local.data as T, requestId }
    const token = window.localStorage.getItem('uam-companion-token')
    if (!token) return { ok: false, error: 'Connect to UAM first.', requestId }
    let transferFailure = ''
    try {
      const body = JSON.stringify(envelope)
      const bytes = new TextEncoder().encode(body)
      let response: Response
      if (bytes.length < COMPANION_DIRECT_REQUEST_BYTES) response = await companionPost(token, body)
      else {
        const upload = await uploadCompanionRequest(token, request.action, bytes)
        if ('error' in upload) {
          if (logFailures) console.error(`[CEF] ${request.action}: ${upload.error}`)
          return { ok: false, error: upload.error, requestId }
        }
        response = upload.response
      }
      if (!response.ok) {
        const error = await companionFailure(response)
        if (logFailures) console.error(`[CEF] ${request.action}: ${error}`)
        return { ok: false, error, requestId }
      }
      let data = await response.json()
      if (data?.uamTransfer) {
        const { id, totalBytes, chunkBytes } = data.uamTransfer
        if (typeof id !== 'string' || !Number.isSafeInteger(totalBytes) || totalBytes <= 0) {
          throw new Error('Invalid response transfer.')
        }
        if (totalBytes > 32 * 1024 * 1024) {
          transferFailure = 'Response is too large to load on this device.'
          throw new Error(transferFailure)
        }
        const transferChunkBytes = chunkBytes === undefined ? 128 * 1024 : chunkBytes
        if (!Number.isSafeInteger(transferChunkBytes) || transferChunkBytes <= 0) throw new Error('Invalid response transfer.')
        const bytes = new Uint8Array(totalBytes)
        const chunkCount = Math.ceil(totalBytes / transferChunkBytes)
        const loadChunk = async (offset: number) => {
          if (window.localStorage.getItem('uam-companion-token') !== token) throw new Error('Disconnected.')
          const chunkResponse = await companionPost(token, JSON.stringify({ action: 'getCompanionResponseChunk', payload: { id, offset } }))
          if (!chunkResponse.ok) {
            transferFailure = (await chunkResponse.json().catch(() => null))?.error || 'Could not load the rest of this response.'
            throw new Error(transferFailure)
          }
          const chunk = await chunkResponse.json()
          if (typeof chunk.base64 !== 'string') throw new Error('Invalid response chunk.')
          const decoded = Uint8Array.from(atob(chunk.base64), (character) => character.charCodeAt(0))
          const expectedLength = Math.min(transferChunkBytes, totalBytes - offset)
          if (decoded.length !== expectedLength || chunk.nextOffset !== offset + decoded.length || chunk.nextOffset > totalBytes
            || chunk.done !== (chunk.nextOffset === totalBytes)) throw new Error('Incomplete response chunk.')
          bytes.set(decoded, offset)
        }
        // Leave the final chunk until last: the server deletes the transfer when it serves it.
        for (let first = 0; first < chunkCount - 1; first += 4) {
          const offsets = Array.from({ length: Math.min(4, chunkCount - 1 - first) }, (_, index) => (first + index) * transferChunkBytes)
          await Promise.all(offsets.map(loadChunk))
        }
        await loadChunk((chunkCount - 1) * transferChunkBytes)
        if (window.localStorage.getItem('uam-companion-token') !== token) throw new Error('Disconnected.')
        data = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes))
      }
      return data && typeof data.ok === 'boolean'
        ? { ...data as CEFResponse<T>, requestId }
        : { ok: true, data: data as T, requestId }
    } catch {
      return { ok: false, error: transferFailure || 'Connection lost. Check the chat before retrying; the action may have reached UAM.', requestId }
    }
  }

  if (typeof window !== 'undefined' && typeof window.cefQuery === 'function') {
    // Production path — real CEF
    return new Promise<CEFResponse<T>>((resolve) => {
      window.cefQuery!({
        request: JSON.stringify(envelope),
        onSuccess: (response) => {
          try {
            const parsed = JSON.parse(response)
            // C++ handlers return the raw payload as the success body (not wrapped
            // in {ok, data}).  Detect the wrapper format vs raw data: if the parsed
            // object explicitly carries a boolean `ok` field it's already wrapped;
            // otherwise treat the whole object as `data`.
            if (parsed && typeof parsed.ok === 'boolean') {
              resolve({ ...(parsed as CEFResponse<T>), requestId })
            } else {
              resolve({ ok: true, data: parsed as T, requestId })
            }
          } catch {
            // Non-JSON string response
            resolve({ ok: true, data: response as T, requestId })
          }
        },
        onFailure: (code, message) => {
          if (logFailures) console.error(`[CEF] Error ${code}: ${message}`)
          resolve({ ok: false, error: message, code, requestId })
        },
      })
    }).catch((cause: unknown) => {
      const error = cause instanceof Error ? cause.message : 'The CEF request could not be sent.'
      if (logFailures) console.error(`[CEF] Error: ${error}`)
      return { ok: false, error, requestId }
    })
  }

  // Dev/mock path
  const isDev = (import.meta as ImportMeta & { env?: { DEV?: boolean } }).env?.DEV === true
  if (isDev) {
    console.debug('[CEF stub] Request:', redactedEnvelope(envelope))
  }
  await new Promise((r) => setTimeout(r, 80))
  const mock: CEFResponse<T> = { ok: true, data: null as T, requestId }
  if (isDev) {
    console.debug('[CEF stub] Response:', mock)
  }
  return mock
}

/**
 * Bounds the UI wait for a read that is safe to repeat. The native request is not cancelled; a reply
 * arriving after the deadline is ignored. Never use it for actions whose lost acknowledgement would
 * make a retry duplicate work (chat creation, scan start).
 */
export async function boundedRead<T = unknown>(reply: Promise<CEFResponse<T>>, timeoutError: string, timeoutMs = 30_000): Promise<CEFResponse<T>> {
  let deadline: ReturnType<typeof setTimeout> | undefined
  try {
    return await Promise.race([
      reply,
      new Promise<CEFResponse<T>>((resolve) => { deadline = setTimeout(() => resolve({ ok: false, error: timeoutError }), timeoutMs) }),
    ])
  } catch (error) {
    // Only the deadline reports the timeout; a rejected reply keeps its own reason, or none so the caller's fallback shows.
    const reason = error instanceof Error ? error.message : typeof error === 'string' ? error : ''
    return reason ? { ok: false, error: reason } : { ok: false }
  } finally {
    clearTimeout(deadline)
  }
}

export async function sendWhenRemoteStopSettles<T = unknown>(request: CEFRequest): Promise<CEFResponse<T>> {
  const reportFailure = (failure: CEFResponse<T>) => {
    console.error(`[CEF] Error: ${failure.error ?? 'Request failed.'}`)
    return failure
  }
  let response: CEFResponse<T> = { ok: false, error: 'The remote stop timed out.' }
  for (let attempt = 0; attempt < 120; attempt += 1) {
    response = await sendToCEF<T>(request, false)
    if (response.ok) return response
    const error = response.error?.toLowerCase() ?? ''
    if (error.includes('unconfirmed') || error.includes('could not be confirmed')) return reportFailure(response)
    if (!error.includes('remote') || !error.includes('stopping')) return reportFailure(response)
    await new Promise((resolve) => setTimeout(resolve, 250))
  }
  return reportFailure({ ...response, error: 'The remote stop timed out.' })
}
