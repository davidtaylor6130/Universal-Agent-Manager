import { useEffect, useState, type ReactNode } from 'react'
import { Brain, ChevronRight, FileText, MessageSquare, Search, Terminal, Users, Wrench } from 'lucide-react'
import type { AcpToolCall, AcpTurnEvent } from '../../store/useAppStore'
import { MarkdownContent } from '../markdown/Markdown'
import { toolDisplayTitle } from './StatusHelpers'
import './conversation.css'

function ConversationTool({ tool, onSelectTool, renderSubAgentHistory }: {
  tool: AcpToolCall
  onSelectTool: (toolId: string) => void
  renderSubAgentHistory?: (tool: AcpToolCall) => ReactNode
}) {
  const [historyOpen, setHistoryOpen] = useState(false)
  const title = toolDisplayTitle(tool)
  const status = (tool.status.trim().toLowerCase() || 'pending').replace(/_/g, ' ')
  const Icon = tool.isSubAgent ? Users : tool.kind === 'read' ? FileText : tool.kind === 'search' ? Search : ['execute', 'shell'].includes(tool.kind) ? Terminal : Wrench
  return <li className="conversation-work__event">
    <button type="button" className="conversation-work__tool" title={title} onClick={() => onSelectTool(tool.id)} aria-label={`${title}, ${status}`}>
      <Icon size={15} className="conversation-work__event-icon" aria-hidden />
      <span className="conversation-work__tool-title">{title}</span>
      <span className="conversation-work__status" data-failed={['failed', 'error'].includes(status) || undefined}>{status}</span>
    </button>
    {tool.isSubAgent && renderSubAgentHistory && <details className="conversation-work__subagent" open={historyOpen}>
      <summary onClick={(event) => { event.preventDefault(); setHistoryOpen((current) => !current) }}>Sub-agent transcript <ChevronRight size={12} aria-hidden /></summary>
      {historyOpen && <div className="conversation-work__history">{renderSubAgentHistory(tool)}</div>}
    </details>}
  </li>
}

/** Presentation only: tool detail and sub-agent callbacks remain owned by the chat. */
export function ConversationWork({ events, tools, active, duration, onSelectTool, renderSubAgentHistory, prioritySteer }: {
  events: AcpTurnEvent[]
  tools: AcpToolCall[]
  active: boolean
  duration: string
  onSelectTool: (toolId: string) => void
  renderSubAgentHistory?: (tool: AcpToolCall) => ReactNode
  prioritySteer?: ReactNode
}) {
  const [open, setOpen] = useState(active)
  useEffect(() => { setOpen(active) }, [active])
  if (events.length === 0 && tools.length === 0 && !prioritySteer) return null

  const toolById = new Map(tools.map((tool) => [tool.id, tool]))
  const referencedTools = new Set(events.flatMap((event) => event.type === 'tool_call' ? [event.toolCallId] : []))
  return <details className="conversation-work" data-testid="working-summary" data-active={active} open={open}>
    <summary className="conversation-work__summary" onClick={(event) => { event.preventDefault(); setOpen((current) => !current) }}>
      <span className="conversation-work__heading"><span>{active ? 'Working' : `Worked for ${duration}`}</span><ChevronRight size={14} className="conversation-work__chevron" aria-hidden /></span>
      <span className="conversation-work__divider" aria-hidden />
    </summary>
    {open && <div className="conversation-work__expanded">
      <ol className="conversation-work__timeline" aria-label="Work activity">
        {events.map((event, index) => {
          if (event.type === 'tool_call') {
            const tool = toolById.get(event.toolCallId) ?? {
              id: event.toolCallId, title: event.toolCallId, kind: 'tool', status: 'pending', content: '',
            }
            return <ConversationTool key={`tool-${tool.id}-${index}`} tool={tool} onSelectTool={onSelectTool} renderSubAgentHistory={renderSubAgentHistory} />
          }
          if ((event.type === 'thought' || event.type === 'assistant_text') && event.text.trim()) {
            const Icon = event.type === 'thought' ? Brain : MessageSquare
            return <li key={`text-${index}`} className="conversation-work__event conversation-work__note">
              <Icon size={15} className="conversation-work__event-icon" aria-hidden />
              <div className="conversation-work__reasoning"><MarkdownContent content={event.text} /></div>
            </li>
          }
          return null
        })}
        {tools.filter((tool) => !referencedTools.has(tool.id)).map((tool) => <ConversationTool key={`extra-${tool.id}`} tool={tool} onSelectTool={onSelectTool} renderSubAgentHistory={renderSubAgentHistory} />)}
      </ol>
      {prioritySteer}
    </div>}
  </details>
}
