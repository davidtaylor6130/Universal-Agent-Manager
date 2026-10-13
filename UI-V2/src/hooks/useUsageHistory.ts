import { useCallback, useEffect, useState } from 'react'
import { createRequestId, isCefContext, sendToCEF } from '../ipc/cefBridge'
import { parseUsageHistory, type UsageHistory, type UsagePriceOverride, type UsageWindow } from '../services/usageHistory'

interface HistoryState {
  data: UsageHistory | null
  loading: boolean
  error: string
  scope: string
  dataScope: string
}

/** Poll cached accounting snapshots; obsolete replies never replace the selected host/window. */
export function useUsageHistory(executionHostId: string, window: UsageWindow, prices: UsagePriceOverride[] = [], enabled = true) {
  const [refreshIndex, setRefreshIndex] = useState(0)
  const [state, setState] = useState<HistoryState>({ data: null, loading: true, error: '', scope: '', dataScope: '' })
  const priceKey = JSON.stringify(prices)
  const scope = JSON.stringify([executionHostId, window.startMs, window.endMs, window.days, priceKey, refreshIndex])
  const refresh = useCallback(() => setRefreshIndex(index => index + 1), [])
  useEffect(() => {
    if (!enabled) return
    let obsolete = false
    let timer: ReturnType<typeof setTimeout> | undefined
    let timeout: ReturnType<typeof setTimeout> | undefined
    const refreshId = createRequestId('usage')
    setState(previous => ({ ...previous, loading: true, error: '', scope }))
    const load = async () => {
      try {
        if (!isCefContext()) throw new Error('Usage history is available in the desktop app. This preview has no provider history.')
        const reply = await Promise.race([
          sendToCEF({ action: 'getUsageHistory', payload: { refreshId, executionHostId, startMs: window.startMs, endMs: window.endMs, windowDays: window.days, priceOverrides: JSON.parse(priceKey) } }, false),
          new Promise<never>((_, reject) => { timeout = setTimeout(() => reject(new Error('Usage history took too long to load. Refresh to try again.')), 30000) }),
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
        setState({ data, loading: data.version === 3 ? Boolean(data.collecting) : !data.complete, error: data.collectionError || '', scope, dataScope: scope })
        timer = setTimeout(() => { void load() }, (data.collecting || data.pricing?.refreshing) ? 2000 : 30000)
      } catch (error) {
        clearTimeout(timeout)
        if (!obsolete) setState(previous => ({ ...previous, loading: false, scope, error: error instanceof Error ? error.message : 'Could not load usage history.' }))
      }
    }
    void load()
    return () => { obsolete = true; clearTimeout(timer); clearTimeout(timeout) }
  }, [executionHostId, window.startMs, window.endMs, window.days, priceKey, scope, enabled])
  const current = state.scope === scope
  const kept = state.data !== null && state.dataScope !== scope
  return { data: state.data, loading: enabled && (!current || state.loading), error: current ? state.error : '', kept, refresh }
}
