export type Role = 'user' | 'assistant' | 'system'

export interface Attachment {
  id: string
  name: string
  type: string
  size: number
}

export interface Message {
  id: string
  sessionId: string
  role: Role
  content: string
  thoughts?: string
  isStreaming?: boolean
  attachments?: Attachment[]
  createdAt: Date
}
