export type Role = 'user' | 'assistant' | 'system'

export interface Attachment {
  id: string
  name: string
  type: string
  size: number
}

export interface MessageToolCall {
  id: string
  title: string
  kind: string
  status: string
  content: string
}

export interface Message {
  id: string
  sessionId: string
  role: Role
  content: string
  thoughts?: string
  toolCalls?: MessageToolCall[]
  isStreaming?: boolean
  attachments?: Attachment[]
  createdAt: Date
}
