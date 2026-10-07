export interface ChatSearchHit {
  chatId: string
  messageIndex: number
  messagesDigest: string
  field: 'content' | 'thoughts' | 'planSummary'
  excerpt: string
}

export interface ChatSearchNavigation {
  generation: number
  queryRequestId: string
  chatId: string
  messageIndex: number
  messagesDigest: string
  phase: 'loading' | 'ready'
  anchored: boolean
}

export interface SearchChatMessagesResponse {
  chatIds?: string[]
  hits?: ChatSearchHit[]
}
