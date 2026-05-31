export type GoalStatus = 'active' | 'complete' | 'blocked'

export interface Goal {
  id: string
  chatId: string
  objective: string
  status: GoalStatus
  tokenBudget?: number
  tokensUsed?: number
  blockedTurnCount?: number
  lastBlocker?: string
  createdAt: Date
  updatedAt: Date
}