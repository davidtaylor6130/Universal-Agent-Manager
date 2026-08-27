import type { Goal, GoalStatus } from '../../types/goal'
import { sendToCEF, isCefContext, createRequestId } from '../../ipc/cefBridge'
import type { AppState, ZustandSet, ZustandGet } from '../storeTypes'

export function createGoalsSlice(set: ZustandSet, get: ZustandGet) {
  return {
    goalsByChatId: {} as Record<string, Goal[]>,
    activeGoalIdByChatId: {} as Record<string, string | null>,
    goalModeByChatId: {} as Record<string, boolean>,
    defaultGoalTokenBudgetByChatId: {} as Record<string, number>,

		setGoal: async (chatId: string, objective: string, tokenBudget = 0, executionOwner: 'uam' | 'provider' = 'uam') => {
		  const trimmedObjective = objective.trim()
		  if (!trimmedObjective) return { ok: false, error: 'Goal objective is required.' }
	      if (isCefContext()) {
	        const response = await sendToCEF<{ goalId: string }>({
	          action: 'setGoal',
			  payload: { chatId, objective: trimmedObjective, tokenBudget, executionOwner },
	          requestId: createRequestId('setGoal'),
	        })
	        if (response.ok && response.data?.goalId) {
	          return { ok: true, goalId: response.data.goalId }
	        }
	        return { ok: false, error: response.error ?? 'Failed to create goal.' }
	      }

      const goalId = `goal-${Date.now()}`
      const now = new Date()
      const newGoal: Goal = {
        id: goalId,
        chatId,
	        objective: trimmedObjective,
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
	      return { ok: true, goalId }
	    },

	    updateGoalStatus: async (chatId: string, goalId: string, status: GoalStatus) => {
	      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'updateGoalStatus',
          payload: { chatId, goalId, status },
          requestId: createRequestId('updateGoalStatus'),
        })
	        return { ok: response.ok, error: response.error }
      }

      set((state: AppState) => ({
        goalsByChatId: {
          ...state.goalsByChatId,
          [chatId]: (state.goalsByChatId[chatId] ?? []).map((goal) =>
            goal.id === goalId ? { ...goal, status, updatedAt: new Date() } : goal
          ),
        },
        activeGoalIdByChatId:
          (status === 'complete' || status === 'blocked' || status === 'paused') &&
          state.activeGoalIdByChatId[chatId] === goalId
            ? { ...state.activeGoalIdByChatId, [chatId]: null }
            : state.activeGoalIdByChatId,
      }))
	      return { ok: true }
	    },

	    updateGoalObjective: async (chatId: string, goalId: string, objective: string) => {
	      const trimmedObjective = objective.trim()
	      if (!trimmedObjective) return { ok: false, error: 'Goal objective is required.' }
	      if (isCefContext()) {
	        const response = await sendToCEF({
	          action: 'updateGoalObjective',
	          payload: { chatId, goalId, objective: trimmedObjective },
	          requestId: createRequestId('updateGoalObjective'),
	        })
	        return { ok: response.ok, error: response.error }
	      }
	      const goal = (get().goalsByChatId[chatId] ?? []).find((candidate) => candidate.id === goalId)
	      if (!goal || goal.status === 'complete' || goal.executionOwner === 'provider') {
	        return { ok: false, error: 'Only non-complete UAM-managed goals can be edited.' }
	      }
	      set((state: AppState) => ({
	        goalsByChatId: {
	          ...state.goalsByChatId,
	          [chatId]: (state.goalsByChatId[chatId] ?? []).map((candidate) =>
	            candidate.id === goalId ? { ...candidate, objective: trimmedObjective, updatedAt: new Date() } : candidate
	          ),
	        },
	      }))
	      return { ok: true }
	    },

	    removeGoal: async (chatId: string, goalId: string) => {
		  if ((get().goalsByChatId[chatId] ?? []).some((goal) => goal.id === goalId && goal.status === 'complete')) return { ok: false, error: 'Completed goals cannot be deleted.' }
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'removeGoal',
          payload: { chatId, goalId },
          requestId: createRequestId('removeGoal'),
        })
	        return { ok: response.ok, error: response.error }
      }

      set((state: AppState) => ({
        goalsByChatId: {
          ...state.goalsByChatId,
          [chatId]: (state.goalsByChatId[chatId] ?? []).filter((goal) => goal.id !== goalId),
        },
        activeGoalIdByChatId: state.activeGoalIdByChatId[chatId] === goalId
          ? { ...state.activeGoalIdByChatId, [chatId]: null }
          : state.activeGoalIdByChatId,
      }))
	      return { ok: true }
	    },

	    resumeGoal: async (chatId: string, goalId: string) => {
	      if (!goalId) return { ok: false, error: 'Goal id is required.' }
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'resumeGoal',
          payload: { chatId, goalId },
          requestId: createRequestId('resumeGoal'),
        })
	        return { ok: response.ok, error: response.error }
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
	      return { ok: true }
	    },

	    clearActiveGoal: async (chatId: string) => {
	      if (isCefContext()) {
	        const currentActiveGoalId = get().activeGoalIdByChatId[chatId]
	        if (!currentActiveGoalId) return { ok: true }
        const response = await sendToCEF({
          action: 'setActiveGoal',
          payload: { chatId, goalId: '' },
          requestId: createRequestId('setActiveGoal'),
        })
	        return { ok: response.ok, error: response.error }
      }

      set((state: AppState) => ({
        activeGoalIdByChatId: {
          ...state.activeGoalIdByChatId,
          [chatId]: null,
        },
      }))
	      return { ok: true }
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
