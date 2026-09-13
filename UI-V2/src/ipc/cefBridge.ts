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
  return typeof window !== 'undefined' && window.location.pathname.replace(/\/$/, '') === '/companion'
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
  requestId?: string
}

export function createRequestId(prefix = 'cef'): string {
  const cryptoObj = typeof globalThis !== 'undefined' ? globalThis.crypto : undefined
  if (cryptoObj && typeof cryptoObj.randomUUID === 'function') {
    return cryptoObj.randomUUID()
  }

  return `${prefix}-${Date.now()}-${Math.random().toString(16).slice(2)}`
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
    const token = window.localStorage.getItem('uam-companion-token')
    if (!token) return { ok: false, error: 'Connect to UAM first.', requestId }
    try {
      const response = await fetch('/api', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${token}` },
        body: JSON.stringify(envelope),
        signal: AbortSignal.timeout(30000),
        redirect: 'error',
        cache: 'no-store',
      })
      let data = await response.json()
      if (!response.ok) {
        const error = data.error ?? `Request failed (${response.status}).`
        if (logFailures) console.error(`[CEF] ${request.action}: ${error}`)
        return { ok: false, error, requestId }
      }
      if (data?.uamTransfer) {
        const { id, totalBytes } = data.uamTransfer
        if (typeof id !== 'string' || !Number.isSafeInteger(totalBytes) || totalBytes <= 0) {
          throw new Error('Invalid response transfer.')
        }
        const bytes = new Uint8Array(totalBytes)
        let offset = 0
        while (offset < totalBytes) {
          if (window.localStorage.getItem('uam-companion-token') !== token) throw new Error('Disconnected.')
          const chunkResponse = await fetch('/api', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${token}` },
            body: JSON.stringify({ action: 'getCompanionResponseChunk', payload: { id, offset } }),
            signal: AbortSignal.timeout(30000),
            redirect: 'error',
            cache: 'no-store',
          })
          const chunk = await chunkResponse.json()
          if (!chunkResponse.ok) return { ok: false, error: chunk.error || 'Could not load the rest of this response.', requestId }
          if (typeof chunk.base64 !== 'string') throw new Error('Invalid response chunk.')
          const decoded = Uint8Array.from(atob(chunk.base64), (character) => character.charCodeAt(0))
          if (!decoded.length || chunk.nextOffset !== offset + decoded.length || chunk.nextOffset > totalBytes
            || chunk.done !== (chunk.nextOffset === totalBytes)) throw new Error('Incomplete response chunk.')
          bytes.set(decoded, offset)
          offset = chunk.nextOffset
        }
        data = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes))
      }
      return data && typeof data.ok === 'boolean'
        ? { ...data as CEFResponse<T>, requestId }
        : { ok: true, data: data as T, requestId }
    } catch {
      return { ok: false, error: 'Connection lost. Check the chat before retrying; the action may have reached UAM.', requestId }
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
          resolve({ ok: false, error: message, requestId })
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
    console.debug('[CEF stub] Request:', envelope)
  }
  await new Promise((r) => setTimeout(r, 80))
  const mock: CEFResponse<T> = { ok: true, data: null as T, requestId }
  if (isDev) {
    console.debug('[CEF stub] Response:', mock)
  }
  return mock
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
