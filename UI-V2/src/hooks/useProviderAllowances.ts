import { useCallback, useEffect, useState, useSyncExternalStore } from 'react'
import { getProviderAllowanceState, preloadProviderAllowances, subscribeProviderAllowances } from '../services/providerAllowances'

/** Account snapshots belong to a computer, independently of its chats and selected session. */
export function useProviderAllowances(executionHostId: string) {
  const subscribe = useCallback((listener: () => void) => subscribeProviderAllowances(executionHostId, listener), [executionHostId])
  const read = useCallback(() => getProviderAllowanceState(executionHostId), [executionHostId])
  const state = useSyncExternalStore(subscribe, read, read)
  const [clock, setClock] = useState(() => Date.now())
  const coolingDown = state.cooldownUntil > Math.max(clock, Date.now())
  useEffect(() => { void preloadProviderAllowances(executionHostId) }, [executionHostId])
  useEffect(() => {
    if (executionHostId !== 'local') return
    const remainingCooldown = Math.max(0, state.cooldownUntil - Date.now())
    const interval = Math.min(state.refreshing ? 1000 : 30000, remainingCooldown || 30000)
    const timer = setTimeout(() => { setClock(Date.now()); void preloadProviderAllowances(executionHostId) }, interval)
    return () => clearTimeout(timer)
  }, [executionHostId, state, clock])
  const refresh = useCallback(() => { if (executionHostId === 'local') void preloadProviderAllowances(executionHostId, true) }, [executionHostId])
  return { ...state, coolingDown, refresh }
}
