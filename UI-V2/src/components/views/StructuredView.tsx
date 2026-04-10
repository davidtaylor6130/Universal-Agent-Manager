import { useAppStore } from '../../store/useAppStore'
import { MessageList } from '../chat/MessageList'
import { InputBar } from '../input/InputBar'
import { ProviderChiplets } from '../input/ProviderChiplets'
import { Session } from '../../types/session'

interface StructuredViewProps {
  session: Session
}

export function StructuredView({ session }: StructuredViewProps) {
  const { messages, streamingMessageId, sendMessage } = useAppStore()
  const sessionMessages = messages[session.id] ?? []
  const isStreaming = sessionMessages.some((m) => m.isStreaming)

  return (
    <div className="flex flex-col h-full overflow-hidden">
      {/* Message list */}
      <MessageList
        messages={sessionMessages}
        streamingMessageId={streamingMessageId}
      />

      {/* Provider chiplets */}
      <div
        className="flex-shrink-0 px-4 py-2"
        style={{ borderTop: '1px solid var(--border)', background: 'var(--surface)' }}
      >
        <ProviderChiplets sessionId={session.id} />
      </div>

      {/* Input bar */}
      <InputBar
        onSend={(content) => sendMessage(session.id, content)}
        disabled={isStreaming}
        placeholder={isStreaming ? 'Waiting for response…' : 'Message…'}
      />
    </div>
  )
}
