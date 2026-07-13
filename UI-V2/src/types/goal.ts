export type GoalStatus = 'active' | 'complete' | 'blocked' | 'paused'

export interface Goal {
  id: string
  chatId: string
  objective: string
  status: GoalStatus
  tokenBudget?: number
  tokensUsed?: number
  blockedTurnCount?: number
  lastBlocker?: string
  lastDiagnostic?: string
  completedItems?: string[]
  remainingItems?: string[]
  currentStep?: string
  lastVerification?: string
  lastNextPrompt?: string
  sameNextPromptCount?: number
  loopCount?: number
  createdAt: Date
  updatedAt: Date
}
