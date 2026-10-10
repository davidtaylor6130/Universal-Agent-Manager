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

/** Load sequential background scan batches; obsolete replies never replace the selected host/window. */
export function useUsageHistory(executionHostId: string, window: UsageWindow, prices: UsagePriceOverride[] = [], enabled = true) {
  const [refreshIndex, setRefreshIndex] = useState(0)
  const [state, setState] = useState<HistoryState>({ data: null, loading: true, error: '', scope: '', dataScope: '' })
  const priceKey = JSON.stringify(prices)
  const scope = JSON.stringify([executionHostId, window.startMs, window.endMs, priceKey, refreshIndex])
  const refresh = useCallback(() => setRefreshIndex(index => index + 1), [])
  useEffect(() => {
    if (!enabled) return
    let obsolete = false
    let timer: ReturnType<typeof setTimeout> | undefined
    let timeout: ReturnType<typeof setTimeout> | undefined
    const refreshId = createRequestId('usage')
    let previousProgress = ''
    let stalled = 0
    let batches = 0
    setState(previous => ({ ...previous, loading: true, error: '', scope }))
    const load = async () => {
      try {
        if (!isCefContext()) throw new Error('Usage history is available in the desktop app. This preview has no provider history.')
        const reply = await Promise.race([
          sendToCEF({ action: 'getUsageHistory', payload: { refreshId, executionHostId, startMs: window.startMs, endMs: window.endMs, priceOverrides: JSON.parse(priceKey) } }, false),
          new Promise<never>((_, reject) => { timeout = setTimeout(() => reject(new Error('Usage history took too long to load. Refresh to try again.')), 30000) }),
        ])
        clearTimeout(timeout)
        if (obsolete) return
        if (!reply.ok) throw new Error(reply.error || 'Could not load usage history.')
        const data = parseUsageHistory(reply.data, executionHostId, window)
        setState({ data, loading: !data.complete, error: '', scope, dataScope: scope })
        if (data.complete) return
        const progress = JSON.stringify([data.filesChecked, data.filesTotal, data.totals.records])
        stalled = progress === previousProgress && data.bytesRead === 0 ? stalled + 1 : 0
        previousProgress = progress
        if (stalled >= 2 || ++batches >= 1024) throw new Error('Usage loading stopped before all history was scanned. Refresh to continue.')
        timer = setTimeout(() => { void load() }, 0)
      } catch (error) {
        clearTimeout(timeout)
        if (!obsolete) setState(previous => ({ ...previous, loading: false, scope, error: error instanceof Error ? error.message : 'Could not load usage history.' }))
      }
    }
    void load()
    return () => { obsolete = true; clearTimeout(timer); clearTimeout(timeout) }
  }, [executionHostId, window.startMs, window.endMs, priceKey, scope, enabled])
  const current = state.scope === scope
  const kept = state.data !== null && state.dataScope !== scope
  return { data: state.data, loading: enabled && (!current || state.loading), error: current ? state.error : '', kept, refresh }
}
