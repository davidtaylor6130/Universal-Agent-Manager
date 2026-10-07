import { isCefContext, sendToCEF } from '../../ipc/cefBridge'
import { routingConfigurationError, routingReservation, type ExecutionRoutingSettings, type RoutingReservation } from '../../types/executionRouting'
import type { ZustandGet } from '../storeTypes'

/** Native pushes own routing settings and reservation status; failed writes never invent successful local state. */
export function createExecutionRoutingSlice(get: ZustandGet) {
  return {
    executionRouting: { enabled: false, hosts: [] } as ExecutionRoutingSettings,
    setExecutionRoutingSettings: async (settings: ExecutionRoutingSettings): Promise<void> => {
      const state = get()
      const error = routingConfigurationError(settings, state.executionHosts, state.folders)
      if (error) throw new Error(error)
      if (!isCefContext()) throw new Error('Routing settings require a connected UAM instance.')
      const response = await sendToCEF({ action: 'setExecutionRoutingSettings', payload: settings })
      if (!response.ok) throw new Error(response.error || 'Routing settings could not be saved.')
    },
    releaseExecutionRoutingReservation: async (expected: RoutingReservation): Promise<void> => {
      const session = get().sessions.find((item) => item.id === expected.chatId)
      const current = session && routingReservation(session)
      if (!current || Object.keys(current).some((key) => current[key as keyof RoutingReservation] !== expected[key as keyof RoutingReservation])) throw new Error('The chat assignment changed. Review it again before releasing it.')
      if (!isCefContext()) throw new Error('Releasing a reservation requires a connected UAM instance.')
      const response = await sendToCEF({ action: 'releaseExecutionRoutingReservation', payload: { ...expected, acknowledgeUnresolvedOwnership: true } })
      if (!response.ok) throw new Error(response.error || 'The reservation could not be released.')
    },
  }
}
