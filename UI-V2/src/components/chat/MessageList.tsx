import { useEffect, useRef } from 'react'
import { Message } from '../../types/message'
import { MessageBubble } from './MessageBubble'

interface MessageListProps {
  messages: Message[]
  streamingMessageId: string | null
}

export function MessageList({ messages, streamingMessageId }: MessageListProps) {
  const bottomRef = useRef<HTMLDivElement>(null)
  const containerRef = useRef<HTMLDivElement>(null)

  // Auto-scroll to bottom when messages change or streaming updates
  useEffect(() => {
    const container = containerRef.current
    if (!container) return
    const isNearBottom =
      container.scrollHeight - container.scrollTop - container.clientHeight < 120
    if (isNearBottom || streamingMessageId) {
      bottomRef.current?.scrollIntoView({ behavior: 'smooth', block: 'end' })
    }
  }, [messages, streamingMessageId])

  if (messages.length === 0) {
    return (
      <div className="flex-1 flex flex-col items-center justify-center" style={{ color: 'var(--text-3)' }}>
        <div style={{ fontSize: 32, opacity: 0.2, marginBottom: 12 }}>◈</div>
        <div className="text-sm" style={{ color: 'var(--text-3)' }}>Start the conversation</div>
        <div className="text-xs mt-1" style={{ color: 'var(--text-3)', opacity: 0.6 }}>
          Type a message below
        </div>
      </div>
    )
  }

  return (
    <div
      ref={containerRef}
      className="flex-1 overflow-y-auto py-4"
    >
      {messages.map((msg) => (
        <MessageBubble key={msg.id} message={msg} />
      ))}
      <div ref={bottomRef} style={{ height: 1 }} />
    </div>
  )
}
