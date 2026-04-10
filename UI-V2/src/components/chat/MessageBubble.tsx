import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'
import rehypeHighlight from 'rehype-highlight'
import 'highlight.js/styles/github-dark.css'
import { Message } from '../../types/message'
import { StreamingCursor } from './StreamingCursor'

interface MessageBubbleProps {
  message: Message
}

export function MessageBubble({ message }: MessageBubbleProps) {
  const isUser = message.role === 'user'

  if (isUser) {
    return (
      <div className="flex justify-end mb-4 animate-slide-in px-4">
        <div
          className="max-w-[72%] rounded-2xl rounded-br-sm px-4 py-2.5 text-sm"
          style={{
            background: 'var(--accent)',
            color: '#fff',
            wordBreak: 'break-word',
            lineHeight: '1.55',
          }}
        >
          {/* User messages are plain text */}
          <div style={{ whiteSpace: 'pre-wrap' }}>{message.content}</div>

          {/* Attachments */}
          {message.attachments && message.attachments.length > 0 && (
            <div className="mt-2 flex flex-wrap gap-1">
              {message.attachments.map((a) => (
                <span
                  key={a.id}
                  className="text-xs rounded px-2 py-0.5"
                  style={{ background: 'rgba(0,0,0,0.2)' }}
                >
                  📎 {a.name}
                </span>
              ))}
            </div>
          )}
        </div>
      </div>
    )
  }

  // Assistant message
  return (
    <div className="flex justify-start mb-4 animate-slide-in px-4">
      {/* Avatar */}
      <div
        className="flex-shrink-0 rounded-full flex items-center justify-center mr-2.5 mt-0.5"
        style={{
          width: 22,
          height: 22,
          background: 'var(--accent-dim)',
          border: '1px solid var(--accent)',
          color: 'var(--accent)',
          fontSize: 10,
          fontWeight: 700,
        }}
      >
        ◈
      </div>

      <div
        className="max-w-[80%] rounded-2xl rounded-tl-sm px-4 py-2.5 text-sm"
        style={{
          background: 'var(--surface-up)',
          border: '1px solid var(--border)',
          wordBreak: 'break-word',
          color: 'var(--text)',
          lineHeight: '1.55',
        }}
      >
        {message.content || message.isStreaming ? (
          <div className="prose-msg">
            {message.content ? (
              <ReactMarkdown
                remarkPlugins={[remarkGfm]}
                rehypePlugins={[rehypeHighlight]}
              >
                {message.content}
              </ReactMarkdown>
            ) : null}
            {message.isStreaming && <StreamingCursor />}
          </div>
        ) : (
          <span style={{ color: 'var(--text-3)' }}>…</span>
        )}
      </div>
    </div>
  )
}
