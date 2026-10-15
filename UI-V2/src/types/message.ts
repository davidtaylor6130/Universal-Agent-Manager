export type Role = 'user' | 'assistant' | 'system'

export interface Attachment {
  id: string
  name: string
  type: string
  size: number
  path?: string
  mimeType?: string
  previewKey?: string
  previewOriginPath?: string
}

export interface MessageToolCall {
  id: string
  title: string
  kind: string
  status: string
  approvalStatus?: string
  content: string
  contentDeferred?: boolean
  contentDigest?: string
  isSubAgent?: boolean
  subAgentId?: string
  subAgentTitle?: string
  currentActivity?: string
}

export interface MessagePlanEntry {
  content: string
  priority: string
  status: string
}

export type MessageBlock =
  | { type: 'assistant_text'; text: string; toolCallId?: string; requestId?: string }
  | { type: 'thought'; text: string; toolCallId?: string; requestId?: string }
  | { type: 'context_compaction' | 'context_compaction_interrupted'; text: string; toolCallId?: string; requestId?: string; tokensBefore?: number; tokensAfter?: number }
  | { type: 'plan'; text?: string; toolCallId?: string; requestId?: string }
  | { type: 'tool_call'; toolCallId: string; text?: string; requestId?: string }
  | { type: 'permission_request'; requestId: string; toolCallId?: string; text?: string }
  | { type: 'user_input_request'; requestId: string; toolCallId?: string; text?: string }

export interface Message {
  id: string
  sessionId: string
  role: Role
  content: string
  providerId?: string
  modelId?: string
  thoughts?: string
  planSummary?: string
  planEntries?: MessagePlanEntry[]
  toolCalls?: MessageToolCall[]
  blocks?: MessageBlock[]
  isStreaming?: boolean
  streamMessageIndex?: number
  nativeMessageIndex?: number
  attachments?: Attachment[]
  processingTimeMs?: number
	interrupted?: boolean
  stopReason?: string
	acpPromptNotSent?: boolean
	acpPromptDiscarded?: boolean
	prioritySteer?: boolean
	continuesTurn?: boolean
  checkpointSha?: string
  checkpointParentSha?: string
  createdAt: Date
}
