import { useCallback, useEffect, useState } from 'react'
import { createRequestId, isCefContext, sendToCEF } from '../ipc/cefBridge'
import { assembleUsageHistory, parseUsageHistory, type UsageHistory, type UsagePriceOverride, type UsageWindow } from '../services/usageHistory'

interface HistoryState {
  data: UsageHistory | null
  loading: boolean
  error: string
  scope: string
  dataScope: string
}

// The last answer per host, period and price set, kept for the app session so reopening Usage or
// returning to a period shows it at once while a fresh answer loads in the background.
// ponytail: in-memory only; after a restart the native ledger answers the first request.
const cache = new Map<string, UsageHistory>()
const CACHE_LIMIT = 24
export function clearUsageHistoryCache() { cache.clear() }

/** Poll cached accounting snapshots; obsolete replies never replace the selected host/window. */
export function useUsageHistory(executionHostId: string, window: UsageWindow, prices: UsagePriceOverride[] = [], enabled = true) {
  const [refreshIndex, setRefreshIndex] = useState(0)
  const priceKey = JSON.stringify(prices)
  const scope = JSON.stringify([executionHostId, window.startMs, window.endMs, window.days, priceKey, refreshIndex])
  // A named period is the same question whenever it is asked; native answers it from its own clock.
  const cacheKey = JSON.stringify([executionHostId, window.days ?? [window.startMs, window.endMs], priceKey])
  const [state, setState] = useState<HistoryState>(() => {
    const cached = enabled ? cache.get(cacheKey) : undefined
    return { data: cached ?? null, loading: true, error: '', scope: cached ? scope : '', dataScope: cached ? scope : '' }
  })
  const refresh = useCallback(() => setRefreshIndex(index => index + 1), [])
  useEffect(() => {
    if (!enabled) return
    let obsolete = false
    let timer: ReturnType<typeof setTimeout> | undefined
    let timeout: ReturnType<typeof setTimeout> | undefined
    const refreshId = createRequestId('usage')
    const cached = cache.get(cacheKey)
    setState(previous => cached ? { data: cached, loading: true, error: '', scope, dataScope: scope } : { ...previous, loading: true, error: '', scope })
    const load = async () => {
      let expired = false
      try {
        if (!isCefContext()) throw new Error('Usage history is available in the desktop app. This preview has no provider history.')
        const payload = { refreshId, executionHostId, startMs: window.startMs, endMs: window.endMs, windowDays: window.days, priceOverrides: JSON.parse(priceKey) }
        const reply = await Promise.race([
          (async () => {
            const response = await sendToCEF({ action: 'getUsageHistory', payload }, false)
            if (!response.ok) return response
            const data = await assembleUsageHistory(response.data, refreshId, async (snapshotId, offset) => {
              if (obsolete || expired) throw new Error('Usage history loading was cancelled.')
              const continuation = await sendToCEF({ action: 'getUsageHistory', payload: { ...payload, snapshotId, offset } }, false)
              if (!continuation.ok) throw new Error(continuation.error || 'Could not load usage history.')
              return continuation.data
            }, () => obsolete || expired)
            return { ...response, data }
          })(),
          new Promise<never>((_, reject) => { timeout = setTimeout(() => { expired = true; reject(new Error('Usage history took too long to load. Refresh to try again.')) }, 30000) }),
        ])
        clearTimeout(timeout)
        if (obsolete) return
        if (!reply.ok) throw new Error(reply.error || 'Could not load usage history.')
        if (reply.data && typeof reply.data === 'object' && 'pending' in reply.data && reply.data.pending === true) {
          const error = 'collectionError' in reply.data && typeof reply.data.collectionError === 'string' ? reply.data.collectionError : ''
          setState(previous => ({ ...previous, loading: !error, error, scope }))
          timer = setTimeout(() => { void load() }, error ? 30000 : 500)
          return
        }
        const data = parseUsageHistory(reply.data, executionHostId, window)
        cache.delete(cacheKey)
        cache.set(cacheKey, data)
        if (cache.size > CACHE_LIMIT) cache.delete(cache.keys().next().value!)
        setState({ data, loading: data.version === 3 ? Boolean(data.collecting) : !data.complete, error: data.collectionError || '', scope, dataScope: scope })
        timer = setTimeout(() => { void load() }, (data.collecting || data.pricing?.refreshing) ? 2000 : 30000)
      } catch (error) {
        clearTimeout(timeout)
        if (!obsolete) setState(previous => ({ ...previous, loading: false, scope, error: error instanceof Error ? error.message : 'Could not load usage history.' }))
      }
    }
    void load()
    return () => { obsolete = true; clearTimeout(timer); clearTimeout(timeout) }
  }, [executionHostId, window.startMs, window.endMs, window.days, priceKey, scope, cacheKey, enabled])
  const current = state.scope === scope
  const kept = state.data !== null && state.dataScope !== scope
  return { data: state.data, loading: enabled && (!current || state.loading), error: current ? state.error : '', kept, refresh }
}
