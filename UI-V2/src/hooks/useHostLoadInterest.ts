import { useEffect } from 'react'
import { useAppStore } from '../store/useAppStore'

/** Native samples every computer for 30 seconds after each request; asking every 20 seconds keeps it running while shown. */
export const HOST_LOAD_INTEREST_MS = 20_000

/** Keeps live load figures coming while a surface that shows them is mounted. */
export function useHostLoadInterest(active = true) {
  const request = useAppStore((state) => state.requestHostLoad)
  useEffect(() => {
    if (!active) return
    request()
    const timer = window.setInterval(request, HOST_LOAD_INTEREST_MS)
    return () => window.clearInterval(timer)
  }, [active, request])
}
