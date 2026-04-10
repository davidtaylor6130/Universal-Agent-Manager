/**
 * CEF Bridge — Stage 1 STUB
 *
 * In production (Stage 2), this module will detect window.cefQuery
 * and delegate all calls to the C++ CefMessageRouter backend.
 *
 * For now it returns mock responses so the UI can be developed
 * and tested fully in the browser without a C++ backend.
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

/** Returns true when running inside the CEF host. */
export function isCefContext(): boolean {
  return typeof window !== 'undefined' && typeof window.cefQuery === 'function'
}

export interface CEFRequest {
  action: string
  payload?: unknown
}

export interface CEFResponse<T = unknown> {
  ok: boolean
  data?: T
  error?: string
}

/**
 * Send a request to the C++ backend.
 * In dev: returns a mock response after a short delay.
 * In production (CEF): delegates to window.cefQuery().
 */
export async function sendToCEF<T = unknown>(
  request: CEFRequest
): Promise<CEFResponse<T>> {
  if (typeof window !== 'undefined' && typeof window.cefQuery === 'function') {
    // Production path — real CEF
    return new Promise((resolve) => {
      window.cefQuery!({
        request: JSON.stringify(request),
        onSuccess: (response) => {
          try {
            const parsed = JSON.parse(response)
            // C++ handlers return the raw payload as the success body (not wrapped
            // in {ok, data}).  Detect the wrapper format vs raw data: if the parsed
            // object explicitly carries a boolean `ok` field it's already wrapped;
            // otherwise treat the whole object as `data`.
            if (parsed && typeof parsed.ok === 'boolean') {
              resolve(parsed as CEFResponse<T>)
            } else {
              resolve({ ok: true, data: parsed as T })
            }
          } catch {
            // Non-JSON string response
            resolve({ ok: true, data: response as T })
          }
        },
        onFailure: (code, message) => {
          console.error(`[CEF] Error ${code}: ${message}`)
          resolve({ ok: false, error: message })
        },
      })
    })
  }

  // Dev/mock path
  console.debug('[CEF stub] Request:', request)
  await new Promise((r) => setTimeout(r, 80))
  const mock: CEFResponse<T> = { ok: true, data: null as T }
  console.debug('[CEF stub] Response:', mock)
  return mock
}
