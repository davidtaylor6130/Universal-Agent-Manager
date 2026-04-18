import { FormEvent, KeyboardEvent, ReactNode, RefObject, useEffect, useMemo, useRef, useState } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { Session } from '../../types/session'
import {
  useAppStore,
  type AcpBinding,
  type AcpPendingPermission,
  type AcpToolCall,
  type AcpTurnEvent,
} from '../../store/useAppStore'
import type { Message } from '../../types/message'

interface ChatViewProps {
  session: Session
}

function statusLabel(acp?: AcpBinding) {
  if (!acp) return 'Stopped'
  if (acp.lifecycleState === 'waitingPermission') return 'Permission'
  if (acp.processing) return 'Running'
  if (acp.lifecycleState === 'error') return 'Error'
  if (acp.running) return 'Ready'
  return 'Stopped'
}

function statusColor(acp?: AcpBinding) {
  if (!acp) return 'var(--text-3)'
  if (acp.lifecycleState === 'error') return 'var(--red)'
  if (acp.lifecycleState === 'waitingPermission') return 'var(--yellow)'
  if (acp.processing) return 'var(--blue)'
  if (acp.running) return 'var(--green)'
  return 'var(--text-3)'
}

function toolStatusColor(tool: AcpToolCall) {
  if (tool.status === 'completed') return 'var(--green)'
  if (tool.status === 'failed') return 'var(--red)'
  if (tool.status === 'in_progress') return 'var(--blue)'
  return 'var(--text-3)'
}

function roleAccent(role: string) {
  if (role === 'user') return 'var(--accent)'
  if (role === 'assistant') return 'var(--blue)'
  return 'var(--yellow)'
}

function roleLabel(role: string) {
  if (role === 'user') return 'You'
  if (role === 'assistant') return 'Gemini'
  return 'System'
}

function GeminiIcon() {
  return (
    <span
      aria-hidden="true"
      className="inline-flex items-center justify-center"
      style={{
        width: 16,
        height: 16,
        borderRadius: 4,
        background: 'linear-gradient(135deg, #8ab4ff 0%, #c58af9 48%, #4ade80 100%)',
        color: '#ffffff',
        fontSize: 10,
        lineHeight: 1,
      }}
    >
      ✦
    </span>
  )
}

function safeHref(url: string) {
  return /^(https?:|mailto:)/i.test(url) ? url : undefined
}

function renderInlineMarkdown(text: string, keyPrefix: string): ReactNode[] {
  const nodes: ReactNode[] = []
  const pattern = /(`[^`]+`|\*\*[^*]+?\*\*|\[[^\]]+\]\([^)]+\))/g
  let lastIndex = 0
  let match: RegExpExecArray | null

  while ((match = pattern.exec(text)) !== null) {
    if (match.index > lastIndex) {
      nodes.push(text.slice(lastIndex, match.index))
    }

    const token = match[0]
    const key = `${keyPrefix}-${match.index}`
    if (token.startsWith('`')) {
      nodes.push(<code key={key}>{token.slice(1, -1)}</code>)
    } else if (token.startsWith('**')) {
      nodes.push(<strong key={key}>{token.slice(2, -2)}</strong>)
    } else {
      const link = token.match(/^\[([^\]]+)\]\(([^)]+)\)$/)
      const href = link ? safeHref(link[2]) : undefined
      nodes.push(
        href ? (
          <a key={key} href={href} target="_blank" rel="noreferrer">
            {link?.[1]}
          </a>
        ) : (
          <span key={key}>{link?.[1] ?? token}</span>
        )
      )
    }

    lastIndex = match.index + token.length
  }

  if (lastIndex < text.length) {
    nodes.push(text.slice(lastIndex))
  }

  return nodes
}

function MarkdownTextBlock({ text, blockKey }: { text: string; blockKey: string }) {
  const lines = text.replace(/\r\n/g, '\n').split('\n')
  const nodes: ReactNode[] = []
  let paragraph: string[] = []
  let index = 0

  const flushParagraph = () => {
    if (paragraph.length === 0) return
    const content = paragraph.join(' ')
    nodes.push(<p key={`${blockKey}-p-${nodes.length}`}>{renderInlineMarkdown(content, `${blockKey}-p-${nodes.length}`)}</p>)
    paragraph = []
  }

  while (index < lines.length) {
    const line = lines[index]
    const trimmed = line.trim()

    if (!trimmed) {
      flushParagraph()
      index++
      continue
    }

    const heading = trimmed.match(/^(#{1,3})\s+(.+)$/)
    if (heading) {
      flushParagraph()
      const level = heading[1].length
      const content = renderInlineMarkdown(heading[2], `${blockKey}-h-${index}`)
      if (level === 1) nodes.push(<h1 key={`${blockKey}-h-${index}`}>{content}</h1>)
      else if (level === 2) nodes.push(<h2 key={`${blockKey}-h-${index}`}>{content}</h2>)
      else nodes.push(<h3 key={`${blockKey}-h-${index}`}>{content}</h3>)
      index++
      continue
    }

    if (/^[-*]\s+/.test(trimmed)) {
      flushParagraph()
      const items: string[] = []
      while (index < lines.length && /^[-*]\s+/.test(lines[index].trim())) {
        items.push(lines[index].trim().replace(/^[-*]\s+/, ''))
        index++
      }
      nodes.push(
        <ul key={`${blockKey}-ul-${index}`}>
          {items.map((item, itemIndex) => (
            <li key={`${blockKey}-ul-${index}-${itemIndex}`}>
              {renderInlineMarkdown(item, `${blockKey}-ul-${index}-${itemIndex}`)}
            </li>
          ))}
        </ul>
      )
      continue
    }

    if (/^\d+[.)]\s+/.test(trimmed)) {
      flushParagraph()
      const items: string[] = []
      while (index < lines.length && /^\d+[.)]\s+/.test(lines[index].trim())) {
        items.push(lines[index].trim().replace(/^\d+[.)]\s+/, ''))
        index++
      }
      nodes.push(
        <ol key={`${blockKey}-ol-${index}`}>
          {items.map((item, itemIndex) => (
            <li key={`${blockKey}-ol-${index}-${itemIndex}`}>
              {renderInlineMarkdown(item, `${blockKey}-ol-${index}-${itemIndex}`)}
            </li>
          ))}
        </ol>
      )
      continue
    }

    if (/^>\s?/.test(trimmed)) {
      flushParagraph()
      const quoteLines: string[] = []
      while (index < lines.length && /^>\s?/.test(lines[index].trim())) {
        quoteLines.push(lines[index].trim().replace(/^>\s?/, ''))
        index++
      }
      nodes.push(
        <blockquote key={`${blockKey}-quote-${index}`}>
          {renderInlineMarkdown(quoteLines.join(' '), `${blockKey}-quote-${index}`)}
        </blockquote>
      )
      continue
    }

    paragraph.push(trimmed)
    index++
  }

  flushParagraph()
  return <>{nodes}</>
}

function MarkdownContent({ content }: { content: string }) {
  const parts: ReactNode[] = []
  const fencePattern = /```([A-Za-z0-9_-]+)?\n?([\s\S]*?)```/g
  let lastIndex = 0
  let match: RegExpExecArray | null

  while ((match = fencePattern.exec(content)) !== null) {
    if (match.index > lastIndex) {
      parts.push(
        <MarkdownTextBlock
          key={`text-${lastIndex}`}
          blockKey={`text-${lastIndex}`}
          text={content.slice(lastIndex, match.index)}
        />
      )
    }

    const language = match[1]?.trim()
    parts.push(
      <pre key={`code-${match.index}`}>
        {language && <div className="mb-2 text-[10px] uppercase" style={{ color: 'var(--text-3)' }}>{language}</div>}
        <code>{match[2].replace(/\n$/, '')}</code>
      </pre>
    )
    lastIndex = match.index + match[0].length
  }

  if (lastIndex < content.length) {
    parts.push(
      <MarkdownTextBlock
        key={`text-${lastIndex}`}
        blockKey={`text-${lastIndex}`}
        text={content.slice(lastIndex)}
      />
    )
  }

  return <div className="prose-msg">{parts}</div>
}

function ToolCallInlineRows({ tools, onSelectTool }: { tools: AcpToolCall[]; onSelectTool: (toolId: string) => void }) {
  if (tools.length === 0) return null

  return (
    <div className="space-y-1">
      {tools.map((tool) => (
        <button
          key={tool.id}
          type="button"
          onClick={() => onSelectTool(tool.id)}
          className="w-full flex items-center gap-2 text-left"
          style={{
            border: '1px solid var(--border)',
            borderRadius: 6,
            background: 'color-mix(in srgb, var(--surface) 72%, var(--bg))',
            color: 'var(--text-2)',
            padding: '6px 8px',
          }}
          title="Open tool details"
        >
          <span style={{ color: toolStatusColor(tool), fontSize: 9 }}>●</span>
          <span className="text-[11px]" style={{ color: 'var(--text-3)' }}>Tool call:</span>
          <span className="text-xs truncate" style={{ color: 'var(--text)' }}>{tool.title || tool.id}</span>
          {tool.status && <span className="ml-auto text-[11px]" style={{ color: 'var(--text-3)' }}>{tool.status}</span>}
        </button>
      ))}
    </div>
  )
}

function PermissionInlineCard({
  permission,
  onResolve,
}: {
  permission: AcpPendingPermission
  onResolve: (requestId: string, optionId: string) => void
}) {
  return (
    <div
      className="my-2"
      style={{
        border: '1px solid var(--border-bright)',
        borderRadius: 7,
        padding: 10,
        background: 'color-mix(in srgb, var(--surface) 82%, var(--bg))',
      }}
    >
      <div className="text-xs font-semibold mb-1" style={{ color: 'var(--text)' }}>
        {permission.title || 'Permission required'}
      </div>
      {permission.content && (
        <pre
          className="text-[11px] whitespace-pre-wrap mb-2"
          style={{ color: 'var(--text-2)', fontFamily: 'inherit', overflowWrap: 'anywhere' }}
        >
          {permission.content}
        </pre>
      )}
      <div className="flex flex-wrap gap-2">
        {permission.options.map((option) => (
          <button
            key={option.id}
            type="button"
            className="px-3 h-7 text-[11px] font-medium"
            style={{
              borderRadius: 6,
              border: '1px solid var(--border-bright)',
              background: option.kind.startsWith('allow') ? 'var(--accent-dim)' : 'var(--surface-up)',
              color: 'var(--text)',
            }}
            onClick={() => onResolve(permission.requestId, option.id)}
          >
            {option.name || option.id}
          </button>
        ))}
        <button
          type="button"
          className="px-3 h-7 text-[11px]"
          style={{
            borderRadius: 6,
            border: '1px solid var(--border)',
            background: 'transparent',
            color: 'var(--text-2)',
          }}
          onClick={() => onResolve(permission.requestId, 'cancelled')}
        >
          Cancel
        </button>
      </div>
    </div>
  )
}

function ToolCallModal({ tool, onClose }: { tool: AcpToolCall; onClose: () => void }) {
  return (
    <div
      className="absolute inset-0 flex items-center justify-center"
      style={{
        zIndex: 1000,
        background: 'rgba(0, 0, 0, 0.48)',
        padding: 18,
      }}
      onMouseDown={onClose}
    >
      <section
        role="dialog"
        aria-modal="true"
        aria-label="Tool details"
        className="w-full"
        style={{
          maxWidth: 680,
          maxHeight: 'min(720px, 88vh)',
          overflow: 'hidden',
          borderRadius: 8,
          border: '1px solid var(--border-bright)',
          background: 'var(--surface)',
          boxShadow: '0 22px 70px rgba(0, 0, 0, 0.42)',
        }}
        onMouseDown={(event) => event.stopPropagation()}
      >
        <div
          className="flex items-center gap-3 px-4"
          style={{
            minHeight: 44,
            borderBottom: '1px solid var(--border)',
          }}
        >
          <span style={{ color: toolStatusColor(tool), fontSize: 10 }}>●</span>
          <div className="min-w-0 flex-1">
            <div className="text-sm font-semibold truncate" style={{ color: 'var(--text)' }}>
              {tool.title || tool.id}
            </div>
            <div className="text-[11px]" style={{ color: 'var(--text-3)' }}>
              {[tool.kind, tool.status].filter(Boolean).join(' / ') || 'tool call'}
            </div>
          </div>
          <button
            type="button"
            title="Close tool details"
            onClick={onClose}
            className="px-2 h-7 text-xs"
            style={{
              borderRadius: 5,
              border: '1px solid var(--border)',
              background: 'var(--bg)',
              color: 'var(--text-2)',
            }}
          >
            Close
          </button>
        </div>
        <div className="p-4 overflow-auto" style={{ maxHeight: 'calc(min(720px, 88vh) - 44px)' }}>
          <div className="grid gap-2 text-xs mb-4" style={{ color: 'var(--text-2)' }}>
            <div><span style={{ color: 'var(--text-3)' }}>id:</span> {tool.id || 'unknown'}</div>
            <div><span style={{ color: 'var(--text-3)' }}>kind:</span> {tool.kind || 'unknown'}</div>
            <div><span style={{ color: 'var(--text-3)' }}>status:</span> {tool.status || 'unknown'}</div>
          </div>
          <pre
            className="whitespace-pre-wrap text-xs"
            style={{
              border: '1px solid var(--border)',
              borderRadius: 6,
              background: 'var(--bg)',
              color: 'var(--text)',
              padding: 12,
              overflowWrap: 'anywhere',
              fontFamily: 'inherit',
            }}
          >
            {tool.content || 'No tool output yet.'}
          </pre>
        </div>
      </section>
    </div>
  )
}

function MessageFrame({ role, children }: { role: Message['role']; children: ReactNode }) {
  return (
    <div
      className="flex"
      style={{ justifyContent: role === 'user' ? 'flex-end' : 'flex-start' }}
    >
      <article
        className="min-w-0"
        style={{
          maxWidth: role === 'user' ? '78%' : '100%',
          border: role === 'user' ? '1px solid var(--border)' : '1px solid transparent',
          borderLeft: role !== 'user' ? `2px solid ${roleAccent(role)}` : undefined,
          borderRadius: role === 'user' ? 7 : 0,
          padding: role === 'user' ? '9px 11px' : '2px 0 2px 12px',
          background: role === 'user' ? 'color-mix(in srgb, var(--accent-dim) 55%, var(--surface))' : 'transparent',
          color: 'var(--text)',
        }}
      >
        <div className="flex items-center gap-1.5 text-[11px] mb-1" style={{ color: roleAccent(role) }}>
          <span style={{ fontSize: 8 }}>●</span>
          <span>{roleLabel(role)}</span>
        </div>
        {children}
      </article>
    </div>
  )
}

function ThinkingBlock({ text }: { text: string }) {
  if (!text.trim()) return null

  return (
    <details
      className="group"
      style={{
        border: '1px solid color-mix(in srgb, var(--yellow) 35%, var(--border))',
        borderRadius: 7,
        background: 'color-mix(in srgb, var(--yellow) 8%, var(--surface))',
        color: 'var(--text-2)',
        overflow: 'hidden',
      }}
    >
      <summary
        className="flex items-center gap-2 cursor-pointer select-none text-[11px] font-semibold"
        style={{
          minHeight: 30,
          padding: '0 9px',
          color: 'var(--yellow)',
          listStyle: 'none',
        }}
      >
        <span style={{ fontSize: 9 }}>●</span>
        <span>Thinking</span>
        <span className="ml-auto text-[10px]" style={{ color: 'var(--text-3)' }}>
          click to expand
        </span>
      </summary>
      <div
        className="px-3 pb-3 pt-1 text-xs"
        style={{
          borderTop: '1px solid color-mix(in srgb, var(--yellow) 25%, var(--border))',
          color: 'var(--text-2)',
        }}
      >
        <MarkdownContent content={text} />
      </div>
    </details>
  )
}

function PersistedMessageContent({ message }: { message: Message }) {
  const thoughts = message.role === 'assistant' ? message.thoughts?.trim() ?? '' : ''

  if (!thoughts) {
    return <MarkdownContent content={message.content} />
  }

  return (
    <div className="space-y-2">
      <ThinkingBlock text={thoughts} />
      <MarkdownContent content={message.content} />
    </div>
  )
}

function TurnTimelineContent({
  events,
  tools,
  pendingPermission,
  onSelectTool,
  onResolvePermission,
}: {
  events: AcpTurnEvent[]
  tools: AcpToolCall[]
  pendingPermission: AcpPendingPermission | null
  onSelectTool: (toolId: string) => void
  onResolvePermission: (requestId: string, optionId: string) => void
}) {
  const toolById = new Map(tools.map((tool) => [tool.id, tool]))
  const hasPendingPermissionEvent = Boolean(
    pendingPermission &&
      events.some((event) => event.type === 'permission_request' && event.requestId === pendingPermission.requestId)
  )

  return (
    <div className="space-y-2">
      {events.map((event, index) => {
        if (event.type === 'assistant_text') {
          return <MarkdownContent key={`text-${index}`} content={event.text} />
        }

        if (event.type === 'thought') {
          return <ThinkingBlock key={`thought-${index}`} text={event.text} />
        }

        if (event.type === 'tool_call') {
          const tool = toolById.get(event.toolCallId) ?? {
            id: event.toolCallId,
            title: event.toolCallId,
            kind: 'tool',
            status: 'pending',
            content: '',
          }
          const shouldRenderPendingPermission =
            pendingPermission &&
            !hasPendingPermissionEvent &&
            pendingPermission.toolCallId === event.toolCallId

          return (
            <div key={`tool-${event.toolCallId}-${index}`} className="space-y-2">
              <ToolCallInlineRows tools={[tool]} onSelectTool={onSelectTool} />
              {shouldRenderPendingPermission && (
                <PermissionInlineCard permission={pendingPermission} onResolve={onResolvePermission} />
              )}
            </div>
          )
        }

        if (event.type === 'permission_request' && pendingPermission?.requestId === event.requestId) {
          return (
            <PermissionInlineCard
              key={`permission-${event.requestId}-${index}`}
              permission={pendingPermission}
              onResolve={onResolvePermission}
            />
          )
        }

        return null
      })}
    </div>
  )
}

function ComposerToolbar({
  acp,
  elapsedSeconds,
  canSend,
  providerOpen,
  settingsOpen,
  providerMenuRef,
  settingsMenuRef,
  onToggleProvider,
  onToggleSettings,
  onCancel,
}: {
  acp?: AcpBinding
  elapsedSeconds: number
  canSend: boolean
  providerOpen: boolean
  settingsOpen: boolean
  providerMenuRef: RefObject<HTMLDivElement>
  settingsMenuRef: RefObject<HTMLDivElement>
  onToggleProvider: () => void
  onToggleSettings: () => void
  onCancel: () => void
}) {
  const chipStyle = {
    height: 26,
    borderRadius: 6,
    border: '1px solid var(--border)',
    background: 'color-mix(in srgb, var(--surface) 72%, var(--bg))',
    color: 'var(--text-2)',
  }

  return (
    <div
      className="flex items-center gap-2 flex-wrap px-2 py-2 text-xs"
      style={{
        borderTop: '1px solid var(--border)',
        color: 'var(--text-2)',
      }}
    >
      <div ref={providerMenuRef} className="relative">
        <button
          type="button"
          title="Select provider"
          onClick={onToggleProvider}
          className="inline-flex items-center gap-1.5 px-2"
          style={{
            ...chipStyle,
            color: providerOpen ? 'var(--text)' : 'var(--text-2)',
            borderColor: providerOpen ? 'var(--border-bright)' : 'var(--border)',
          }}
        >
          <GeminiIcon />
          <span>Gemini</span>
        </button>
        {providerOpen && (
          <div
            className="absolute left-0"
            style={{
              bottom: 32,
              width: 230,
              zIndex: 40,
              border: '1px solid var(--border-bright)',
              borderRadius: 8,
              background: 'var(--surface)',
              boxShadow: '0 14px 42px rgba(0, 0, 0, 0.28)',
              padding: 6,
            }}
          >
            <div className="px-2 py-1 text-[11px]" style={{ color: 'var(--text-3)' }}>Provider</div>
            <button
              type="button"
              onClick={onToggleProvider}
              className="w-full flex items-center gap-2 text-left px-2 py-2"
              style={{
                borderRadius: 6,
                background: 'var(--accent-dim)',
                color: 'var(--text)',
              }}
            >
              <GeminiIcon />
              <span className="flex-1">Gemini</span>
              <span style={{ color: 'var(--green)', fontSize: 10 }}>●</span>
            </button>
            <div className="px-2 pt-2 pb-1 text-[11px]" style={{ color: 'var(--text-3)' }}>
              Only available provider for this release.
            </div>
          </div>
        )}
      </div>
      <button type="button" title="Runtime" className="inline-flex items-center gap-1.5 px-2" style={chipStyle}>
        <span style={{ color: 'var(--green)', fontSize: 10 }}>●</span>
        <span>ACP</span>
      </button>
      <button type="button" title="Usage" className="inline-flex items-center gap-1.5 px-2" style={chipStyle}>
        <span>Usage</span>
        <span style={{ color: 'var(--text-3)' }}>--</span>
      </button>
      {acp?.processing && (
        <span
          className="inline-flex items-center gap-1.5 px-2"
          style={{
            ...chipStyle,
            color: 'var(--text)',
            borderColor: 'color-mix(in srgb, var(--blue) 40%, var(--border))',
          }}
        >
          <span style={{ color: 'var(--blue)', fontSize: 10 }}>●</span>
          <span>Working {elapsedSeconds}s</span>
        </span>
      )}
      <div className="ml-auto flex items-center gap-2">
        <div ref={settingsMenuRef} className="relative">
          <button
            type="button"
            title="Settings"
            onClick={onToggleSettings}
            className="inline-flex items-center gap-1.5 px-2"
            style={{
              ...chipStyle,
              color: settingsOpen ? 'var(--text)' : 'var(--text-2)',
              borderColor: settingsOpen ? 'var(--border-bright)' : 'var(--border)',
            }}
          >
            <span>⚙</span>
          </button>
          {settingsOpen && (
            <div
              className="absolute right-0"
              style={{
                bottom: 32,
                width: 250,
                zIndex: 40,
                border: '1px solid var(--border-bright)',
                borderRadius: 8,
                background: 'var(--surface)',
                boxShadow: '0 14px 42px rgba(0, 0, 0, 0.28)',
                padding: 10,
              }}
            >
              <div className="text-xs font-semibold mb-2" style={{ color: 'var(--text)' }}>Chat settings</div>
              <div className="grid gap-2 text-[11px]" style={{ color: 'var(--text-2)' }}>
                <div className="flex justify-between gap-3">
                  <span style={{ color: 'var(--text-3)' }}>Provider</span>
                  <span>Gemini</span>
                </div>
                <div className="flex justify-between gap-3">
                  <span style={{ color: 'var(--text-3)' }}>Runtime</span>
                  <span>ACP</span>
                </div>
                <div className="flex justify-between gap-3">
                  <span style={{ color: 'var(--text-3)' }}>Usage</span>
                  <span>Unavailable</span>
                </div>
              </div>
            </div>
          )}
        </div>
        {acp?.processing ? (
          <button
            type="button"
            title="Cancel turn"
            onClick={onCancel}
            className="h-[26px] px-3 text-xs font-semibold inline-flex items-center"
            style={{
              borderRadius: 6,
              border: '1px solid var(--border-bright)',
              background: 'var(--surface-up)',
              color: 'var(--text)',
            }}
          >
            Stop
          </button>
        ) : (
          <button
            type="submit"
            title="Send prompt"
            disabled={!canSend}
            className="h-[26px] px-3 text-xs font-semibold inline-flex items-center"
            style={{
              borderRadius: 6,
              border: '1px solid var(--border-bright)',
              background: canSend ? 'var(--accent)' : 'var(--surface-up)',
              color: canSend ? '#fff' : 'var(--text-3)',
            }}
          >
            Send
          </button>
        )}
      </div>
    </div>
  )
}

export function ChatView({ session }: ChatViewProps) {
  const [draft, setDraft] = useState('')
  const [submitting, setSubmitting] = useState(false)
  const [selectedToolCallId, setSelectedToolCallId] = useState<string | null>(null)
  const [providerOpen, setProviderOpen] = useState(false)
  const [settingsOpen, setSettingsOpen] = useState(false)
  const [elapsedSeconds, setElapsedSeconds] = useState(0)
  const messages = useAppStore(useShallow((s) => s.messages[session.id] ?? []))
  const folderDirectory = useAppStore((s) =>
    session.folderId ? s.folders.find((folder) => folder.id === session.folderId)?.directory ?? '' : ''
  )
  const acp = useAppStore((s) => s.acpBindingBySessionId[session.id])
  const sendAcpPrompt = useAppStore((s) => s.sendAcpPrompt)
  const cancelAcpTurn = useAppStore((s) => s.cancelAcpTurn)
  const resolveAcpPermission = useAppStore((s) => s.resolveAcpPermission)
  const bottomRef = useRef<HTMLDivElement>(null)
  const providerMenuRef = useRef<HTMLDivElement>(null)
  const settingsMenuRef = useRef<HTMLDivElement>(null)

  const canSend = useMemo(
    () => draft.trim().length > 0 && !submitting && !acp?.processing,
    [draft, submitting, acp?.processing]
  )

  const selectedToolCall = useMemo(
    () => (acp?.toolCalls ?? []).find((tool) => tool.id === selectedToolCallId) ?? null,
    [acp?.toolCalls, selectedToolCallId]
  )

  const turnEvents = acp?.turnEvents ?? []
  const firstTurnEvent = turnEvents.find((event) => event.type === 'assistant_text' ? event.text.length > 0 : true)
  const turnAssistantMessageIndex = acp?.turnAssistantMessageIndex ?? -1
  const turnUserMessageIndex = acp?.turnUserMessageIndex ?? -1
  const turnSerial = acp?.turnSerial ?? 0
  const processingStartedAtMs = acp?.processing ? acp.processingStartedAtMs : null
  const renderTimelineAfterUser =
    turnEvents.length > 0 &&
    turnUserMessageIndex >= 0 &&
    turnUserMessageIndex < messages.length &&
    (turnAssistantMessageIndex < 0 || turnAssistantMessageIndex >= messages.length || firstTurnEvent?.type !== 'assistant_text')
  const renderTimelineAtAssistant =
    turnEvents.length > 0 &&
    !renderTimelineAfterUser &&
    turnAssistantMessageIndex >= 0 &&
    turnAssistantMessageIndex < messages.length

  useEffect(() => {
    bottomRef.current?.scrollIntoView?.({ block: 'end' })
  }, [
    messages.length,
    messages[messages.length - 1]?.content,
    acp?.toolCalls.length,
    acp?.planEntries.length,
    turnEvents.length,
    turnEvents[turnEvents.length - 1]?.type,
    turnEvents[turnEvents.length - 1]?.text,
    turnSerial,
    acp?.lastError,
  ])

  useEffect(() => {
    if (selectedToolCallId && !(acp?.toolCalls ?? []).some((tool) => tool.id === selectedToolCallId)) {
      setSelectedToolCallId(null)
    }
  }, [acp?.toolCalls, selectedToolCallId])

  useEffect(() => {
    setSelectedToolCallId(null)
  }, [turnSerial])

  useEffect(() => {
    const onMouseDown = (event: MouseEvent) => {
      const target = event.target
      if (!(target instanceof Node)) return

      if (providerOpen && providerMenuRef.current && !providerMenuRef.current.contains(target)) {
        setProviderOpen(false)
      }

      if (settingsOpen && settingsMenuRef.current && !settingsMenuRef.current.contains(target)) {
        setSettingsOpen(false)
      }
    }

    const onKeyDown = (event: globalThis.KeyboardEvent) => {
      if (event.key !== 'Escape') return
      setProviderOpen(false)
      setSettingsOpen(false)
      setSelectedToolCallId(null)
    }

    document.addEventListener('mousedown', onMouseDown)
    document.addEventListener('keydown', onKeyDown)
    return () => {
      document.removeEventListener('mousedown', onMouseDown)
      document.removeEventListener('keydown', onKeyDown)
    }
  }, [providerOpen, settingsOpen])

  useEffect(() => {
    if (processingStartedAtMs === null) {
      setElapsedSeconds(0)
      return undefined
    }

    const updateElapsed = () => {
      setElapsedSeconds(Math.max(0, Math.floor((Date.now() - processingStartedAtMs) / 1000)))
    }
    updateElapsed()
    const interval = window.setInterval(updateElapsed, 1000)
    return () => window.clearInterval(interval)
  }, [processingStartedAtMs])

  const submit = async (event?: FormEvent) => {
    event?.preventDefault()
    if (!canSend) return
    const prompt = draft.trim()
    setSubmitting(true)
    const ok = await sendAcpPrompt(session.id, prompt)
    setSubmitting(false)
    if (ok) setDraft('')
  }

  const onComposerKeyDown = (event: KeyboardEvent<HTMLTextAreaElement>) => {
    if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault()
      void submit()
    }
  }

  const pendingPermission = acp?.pendingPermission
  const workspaceDirectory = session.workspaceDirectory?.trim() || folderDirectory.trim()

  return (
    <div className="relative h-full flex overflow-hidden" style={{ background: 'var(--bg)' }}>
      {selectedToolCall && (
        <ToolCallModal tool={selectedToolCall} onClose={() => setSelectedToolCallId(null)} />
      )}
      <div className="flex-1 flex flex-col min-w-0">
        <div className="flex-1 overflow-auto">
          <div className="mx-auto w-full px-4 py-5" style={{ maxWidth: 940 }}>
            <div className="flex items-center gap-2 mb-5 text-xs" style={{ color: 'var(--text-2)' }}>
              <span style={{ color: statusColor(acp), fontSize: 9 }}>●</span>
              <span>{statusLabel(acp)}</span>
              {acp?.agentInfo?.title && (
                <span style={{ color: 'var(--text-3)' }}>{acp.agentInfo.title}</span>
              )}
              {acp?.sessionId && (
                <span className="truncate" style={{ color: 'var(--text-3)' }}>
                  {acp.sessionId}
                </span>
              )}
            </div>

            <div className="space-y-4">
              {messages.map((message, index) => {
                const shouldRenderTimelineAtAssistant = renderTimelineAtAssistant && index === turnAssistantMessageIndex
                const shouldSkipAssistantMessage = renderTimelineAfterUser && index === turnAssistantMessageIndex

                if (shouldSkipAssistantMessage) return null

                return (
                  <div key={message.id} className="space-y-2">
                    <MessageFrame role={message.role}>
                      {shouldRenderTimelineAtAssistant ? (
                        <TurnTimelineContent
                          key={`turn-${turnSerial}-assistant`}
                          events={turnEvents}
                          tools={acp?.toolCalls ?? []}
                          pendingPermission={pendingPermission ?? null}
                          onSelectTool={setSelectedToolCallId}
                          onResolvePermission={(requestId, optionId) => {
                            void resolveAcpPermission(session.id, requestId, optionId)
                          }}
                        />
                      ) : (
                        <PersistedMessageContent message={message} />
                      )}
                    </MessageFrame>
                    {renderTimelineAfterUser && index === turnUserMessageIndex && (
                      <MessageFrame key={`turn-${turnSerial}-after-user`} role="assistant">
                        <TurnTimelineContent
                          key={`turn-${turnSerial}-after-user-content`}
                          events={turnEvents}
                          tools={acp?.toolCalls ?? []}
                          pendingPermission={pendingPermission ?? null}
                          onSelectTool={setSelectedToolCallId}
                          onResolvePermission={(requestId, optionId) => {
                            void resolveAcpPermission(session.id, requestId, optionId)
                          }}
                        />
                      </MessageFrame>
                    )}
                  </div>
                )
              })}
              {turnEvents.length > 0 && !renderTimelineAfterUser && !renderTimelineAtAssistant && (
                <MessageFrame key={`turn-${turnSerial}-fallback`} role="assistant">
                  <TurnTimelineContent
                    key={`turn-${turnSerial}-fallback-content`}
                    events={turnEvents}
                    tools={acp?.toolCalls ?? []}
                    pendingPermission={pendingPermission ?? null}
                    onSelectTool={setSelectedToolCallId}
                    onResolvePermission={(requestId, optionId) => {
                      void resolveAcpPermission(session.id, requestId, optionId)
                    }}
                  />
                </MessageFrame>
              )}
              <div ref={bottomRef} />
            </div>
          </div>
        </div>

        <form
          onSubmit={submit}
          className="flex-shrink-0"
          style={{
            borderTop: '1px solid var(--border)',
            background: 'var(--surface)',
          }}
        >
          <div className="mx-auto p-3" style={{ maxWidth: 940 }}>
            <div
              className="mb-2 flex items-center gap-2 text-[11px]"
              style={{ color: 'var(--text-3)', minWidth: 0 }}
              title={workspaceDirectory || 'No workspace directory selected'}
            >
              <span style={{ color: 'var(--text-2)', flexShrink: 0 }}>Workspace</span>
              <span
                className="truncate"
                style={{
                  border: '1px solid var(--border)',
                  borderRadius: 6,
                  background: 'var(--bg)',
                  color: workspaceDirectory ? 'var(--text-2)' : 'var(--text-3)',
                  padding: '3px 7px',
                  minWidth: 0,
                }}
              >
                {workspaceDirectory || 'No workspace directory selected'}
              </span>
            </div>
            {acp?.lastError && (
              <div
                className="mb-2 text-xs"
                style={{
                  border: '1px solid color-mix(in srgb, var(--red) 45%, var(--border))',
                  borderRadius: 6,
                  padding: '8px 10px',
                  background: 'color-mix(in srgb, var(--red) 10%, var(--surface))',
                  color: 'var(--text)',
                  overflowWrap: 'anywhere',
                }}
              >
                <span style={{ color: 'var(--red)', fontWeight: 600 }}>Gemini ACP error</span>
                <span style={{ color: 'var(--text-2)' }}> · </span>
                {acp.lastError}
              </div>
            )}
            <div
              style={{
                border: '1px solid var(--border-bright)',
                borderRadius: 8,
                background: 'var(--bg)',
                overflow: 'visible',
              }}
            >
            <textarea
              value={draft}
              onChange={(event) => setDraft(event.target.value)}
              onKeyDown={onComposerKeyDown}
              rows={3}
              placeholder="Message Gemini"
              disabled={submitting}
              className="w-full resize-none text-sm"
              style={{
                minHeight: 72,
                maxHeight: 160,
                border: 'none',
                background: 'transparent',
                color: 'var(--text)',
                padding: '10px 12px',
                outline: 'none',
              }}
            />
            <ComposerToolbar
              acp={acp}
              elapsedSeconds={elapsedSeconds}
              canSend={canSend}
              providerOpen={providerOpen}
              settingsOpen={settingsOpen}
              providerMenuRef={providerMenuRef}
              settingsMenuRef={settingsMenuRef}
              onToggleProvider={() => {
                setProviderOpen((value) => !value)
                setSettingsOpen(false)
              }}
              onToggleSettings={() => {
                setSettingsOpen((value) => !value)
                setProviderOpen(false)
              }}
              onCancel={() => void cancelAcpTurn(session.id)}
            />
            </div>
          </div>
        </form>
      </div>
    </div>
  )
}
