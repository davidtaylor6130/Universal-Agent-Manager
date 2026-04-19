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

export interface MessagePlanEntry {
  content: string
  priority: string
  status: string
}

export type MessageBlock =
  | { type: 'assistant_text'; text: string; toolCallId?: string; requestId?: string }
  | { type: 'thought'; text: string; toolCallId?: string; requestId?: string }
  | { type: 'plan'; text?: string; toolCallId?: string; requestId?: string }
  | { type: 'tool_call'; toolCallId: string; text?: string; requestId?: string }
  | { type: 'permission_request'; requestId: string; toolCallId?: string; text?: string }
  | { type: 'user_input_request'; requestId: string; toolCallId?: string; text?: string }

export interface Message {
  id: string
  sessionId: string
  role: Role
  content: string
  thoughts?: string
  planSummary?: string
  planEntries?: MessagePlanEntry[]
  toolCalls?: MessageToolCall[]
  blocks?: MessageBlock[]
  isStreaming?: boolean
  attachments?: Attachment[]
  createdAt: Date
}
