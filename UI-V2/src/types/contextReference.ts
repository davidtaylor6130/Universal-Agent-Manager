export interface ContextReference {
  id: string
  kind: 'message' | 'terminal'
  sourceChatId: string
  sourceMessageIndex?: number
  sourceMessageId?: string
  terminalId?: string
  nativeSessionId?: string
  executionHostId: string
  workspaceDirectory: string
  capturedAt: string
  label: string
  text: string
  truncated: boolean
}
