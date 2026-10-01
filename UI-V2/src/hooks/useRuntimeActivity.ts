import { useCallback, useRef } from 'react'
import { useAppStore } from '../store/useAppStore'
import { isCefContext, sendToCEF } from '../ipc/cefBridge'

/** Records deliberate interaction on a chat pane without persisting keystrokes or treating output as activity. */
export function useRuntimeActivity(chatId: string) {
  const lastSent = useRef(0)
  return useCallback(() => {
    const now = Date.now()
    if (!chatId || !isCefContext() || now - lastSent.current < 1000) return
    lastSent.current = now
    useAppStore.setState((state) => {
      const cli = state.cliBindingBySessionId[chatId]
      const acp = state.acpBindingBySessionId[chatId]
      const restartGrace = <T extends { running: boolean; idleShutdownAtMs?: number; idleShutdownTimeoutSeconds?: number }>(binding: T) => ({
        ...binding, idleCountdownStartsAtMs: now + 60_000,
        idleShutdownAtMs: now + 60_000 + (binding.idleShutdownTimeoutSeconds ?? 600) * 1000,
      })
      return {
        ...(cli?.running && cli.idleShutdownAtMs ? { cliBindingBySessionId: { ...state.cliBindingBySessionId, [chatId]: restartGrace(cli) } } : {}),
        ...(acp?.running && acp.idleShutdownAtMs ? { acpBindingBySessionId: { ...state.acpBindingBySessionId, [chatId]: restartGrace(acp) } } : {}),
      }
    })
    void sendToCEF({ action: 'recordRuntimeActivity', payload: { chatId } })
  }, [chatId])
}
