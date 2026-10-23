export type Role = 'user' | 'assistant' | 'system'

export type AttachmentSource = 'legacy_path' | 'native_inline_pending' | 'owned_native_asset' | 'provider_uri'

export interface Attachment {
  id: string
  name: string
  type: string
  size: number
  path?: string
  mimeType?: string
  previewKey?: string
  previewOriginPath?: string
  /** Display provenance only; preview and delivery resolve the stored attachment identity. */
  source?: AttachmentSource
  sourceUri?: string
  originExecutionHostId?: string
  nativePartKey?: string
  nativeToolCallId?: string
  nativeSourceField?: string
  nativeAssetKey?: string
  contentDigest?: string
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
  /** Native-derived accepted launch correlation; scoped run metadata alone grants actions. */
  managedLaunchRunId?: string
  isSubAgent?: boolean
  subAgentId?: string
  subAgentTitle?: string
  /** Bounded output-only canonical result metadata, independent of deferred tool content. */
  subAgentResultContent?: string
  subAgentChildren?: Array<{ id: string; title?: string; status?: string; startedAtEpochMs?: number; finishedAtEpochMs?: number; durationMs?: number }>
  currentActivity?: string
}

export interface MessagePlanEntry {
  content: string
  priority: string
  status: string
}

export type MessageBlock =
  | { type: 'assistant_text'; text: string; toolCallId?: string; requestId?: string }
  | { type: 'context_compaction_summary'; text: string; toolCallId?: string; requestId?: string }
  | { type: 'thought'; text: string; toolCallId?: string; requestId?: string }
  | { type: 'context_compaction' | 'context_compaction_interrupted'; text: string; toolCallId?: string; requestId?: string; tokensBefore?: number; tokensAfter?: number }
  | { type: 'plan'; text?: string; toolCallId?: string; requestId?: string }
  | { type: 'tool_call'; toolCallId: string; text?: string; requestId?: string }
  | { type: 'permission_request'; requestId: string; toolCallId?: string; text?: string }
  | { type: 'user_input_request'; requestId: string; toolCallId?: string; text?: string }

export interface Message {
  /** Positive historical local OS-sink write fact only; absent/false is unknown, never retry authority. */
  inputWriteCompleted?: boolean
  /** Inert per-input provenance; never admission/delivery/control proof. */
  nativeInputId?: string
  originRequestId?: string
  /** Output-only correlation; scoped native list alone supplies child authority. */
  managedOriginRunId?: string
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
