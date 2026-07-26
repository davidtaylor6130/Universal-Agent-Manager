import type { Goal, GoalStatus } from '../../types/goal'
import { sendToCEF, isCefContext, createRequestId } from '../../ipc/cefBridge'
import type { AppState, ZustandSet, ZustandGet } from '../storeTypes'

export function createGoalsSlice(set: ZustandSet, get: ZustandGet) {
  return {
    goalsByChatId: {} as Record<string, Goal[]>,
    activeGoalIdByChatId: {} as Record<string, string | null>,
    goalModeByChatId: {} as Record<string, boolean>,
    defaultGoalTokenBudgetByChatId: {} as Record<string, number>,

	setGoal: async (chatId: string, objective: string, tokenBudget = 0, executionOwner: 'uam' | 'provider' = 'uam'): Promise<string | null> => {
      if (isCefContext()) {
        const response = await sendToCEF<{ goalId: string }>({
          action: 'setGoal',
		  payload: { chatId, objective, tokenBudget, executionOwner },
          requestId: createRequestId('setGoal'),
        })
        if (response.ok && response.data?.goalId) {
          return response.data.goalId
        }
        return null
      }

      const goalId = `goal-${Date.now()}`
      const now = new Date()
      const newGoal: Goal = {
        id: goalId,
        chatId,
        objective,
        status: 'active',
        tokenBudget: tokenBudget || undefined,
        tokensUsed: 0,
        blockedTurnCount: 0,
        completedItems: [],
        remainingItems: [],
        currentStep: '',
        lastVerification: '',
        lastNextPrompt: '',
        sameNextPromptCount: 0,
        loopCount: 0,
        createdAt: now,
        updatedAt: now,
		executionOwner,
		providerCommand: executionOwner === 'provider' ? get().providers.find((provider) => provider.id === get().sessions.find((session) => session.id === chatId)?.providerId)?.nativeGoalCommand ?? '' : '',
      }
      set((state: AppState) => ({
        goalsByChatId: {
          ...state.goalsByChatId,
          [chatId]: [...(state.goalsByChatId[chatId] ?? []), newGoal],
        },
        activeGoalIdByChatId: {
          ...state.activeGoalIdByChatId,
          [chatId]: goalId,
        },
      }))
      return goalId
    },

    updateGoalStatus: async (goalId: string, status: GoalStatus): Promise<boolean> => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'updateGoalStatus',
          payload: { goalId, status },
          requestId: createRequestId('updateGoalStatus'),
        })
        return response.ok
      }

      set((state: AppState) => {
        const nextActive: Record<string, string | null> = {}
        const nextGoals: Record<string, Goal[]> = {}
        const entries = Object.entries(state.goalsByChatId) as [string, Goal[]][]
        for (const [chatId, goals] of entries) {
          const updated = goals.map((g: Goal) =>
            g.id === goalId ? { ...g, status, updatedAt: new Date() } : g
          )
          if (updated !== goals) {
            nextGoals[chatId] = updated
            if (status === 'complete' || status === 'blocked' || status === 'paused') {
              if (state.activeGoalIdByChatId[chatId] === goalId) {
                nextActive[chatId] = null
              }
            }
          }
        }
        if (Object.keys(nextGoals).length === 0) return state
        return {
          goalsByChatId: { ...state.goalsByChatId, ...nextGoals },
          activeGoalIdByChatId: { ...state.activeGoalIdByChatId, ...nextActive },
        }
      })
      return true
    },

    removeGoal: async (goalId: string): Promise<boolean> => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'removeGoal',
          payload: { goalId },
          requestId: createRequestId('removeGoal'),
        })
        return response.ok
      }

      set((state: AppState) => {
        const nextActive: Record<string, string | null> = {}
        const nextGoals: Record<string, Goal[]> = {}
        const entries = Object.entries(state.goalsByChatId) as [string, Goal[]][]
        for (const [chatId, goals] of entries) {
          const filtered = goals.filter((g: Goal) => g.id !== goalId)
          if (filtered.length !== goals.length) {
            nextGoals[chatId] = filtered
            if (state.activeGoalIdByChatId[chatId] === goalId) {
              nextActive[chatId] = null
            }
          }
        }
        if (Object.keys(nextGoals).length === 0) return state
        return {
          goalsByChatId: { ...state.goalsByChatId, ...nextGoals },
          activeGoalIdByChatId: { ...state.activeGoalIdByChatId, ...nextActive },
        }
      })
      return true
    },

    resumeGoal: async (chatId: string, goalId: string): Promise<boolean> => {
      if (!goalId) return false
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'resumeGoal',
          payload: { chatId, goalId },
          requestId: createRequestId('resumeGoal'),
        })
        return response.ok
      }

      set((state: AppState) => {
        const goals = state.goalsByChatId[chatId] ?? []
        return {
          goalsByChatId: {
            ...state.goalsByChatId,
            [chatId]: goals.map((goal) =>
              goal.id === goalId
                ? { ...goal, status: 'active', blockedTurnCount: 0, lastBlocker: '', updatedAt: new Date() }
                : goal
            ),
          },
          activeGoalIdByChatId: {
            ...state.activeGoalIdByChatId,
            [chatId]: goalId,
          },
        }
      })
      return true
    },

    clearActiveGoal: async (chatId: string): Promise<boolean> => {
      if (isCefContext()) {
        const currentActiveGoalId = get().activeGoalIdByChatId[chatId]
        if (!currentActiveGoalId) return true
        const response = await sendToCEF({
          action: 'setActiveGoal',
          payload: { chatId, goalId: '' },
          requestId: createRequestId('setActiveGoal'),
        })
        return response.ok
      }

      set((state: AppState) => ({
        activeGoalIdByChatId: {
          ...state.activeGoalIdByChatId,
          [chatId]: null,
        },
      }))
      return true
    },

    setGoalMode: (chatId: string, active: boolean) => {
      set((state: AppState) => ({
        goalModeByChatId: {
          ...state.goalModeByChatId,
          [chatId]: active,
        },
      }))
    },

    setDefaultGoalTokenBudget: (chatId: string, tokenBudget: number) => {
      set((state: AppState) => ({
        defaultGoalTokenBudgetByChatId: {
          ...state.defaultGoalTokenBudgetByChatId,
          [chatId]: Math.max(0, Math.floor(Number.isFinite(tokenBudget) ? tokenBudget : 0)),
        },
      }))
    },
  }
}
