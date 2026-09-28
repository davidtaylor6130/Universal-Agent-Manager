import { createContext, useContext, useEffect, useState, type ReactNode } from 'react'
import { Brain, ChevronRight, FileText, MessageSquare, Search, Terminal, Users, Wrench } from 'lucide-react'
import type { AcpToolCall, AcpTurnEvent } from '../../store/useAppStore'
import { useAppStore } from '../../store/useAppStore'
import { MarkdownContent } from '../markdown/Markdown'
import { toolDisplayTitle } from './StatusHelpers'
import './conversation.css'

const SubAgentDisclosureContext = createContext<Set<string> | null>(null)

/** Keep expanded children across live/saved row replacement, only for this mounted chat. */
export function SubAgentDisclosureProvider({ children }: { children: ReactNode }) {
  const [expandedTools] = useState(() => new Set<string>())
  return <SubAgentDisclosureContext.Provider value={expandedTools}>{children}</SubAgentDisclosureContext.Provider>
}

export function useSubAgentDisclosure(toolId: string) {
  const expandedTools = useContext(SubAgentDisclosureContext)
  const [open, setOpen] = useState(() => expandedTools?.has(toolId) ?? false)
  return [open, (next: boolean) => {
    if (next) expandedTools?.add(toolId)
    else expandedTools?.delete(toolId)
    setOpen(next)
  }] as const
}

/** One clock for the currently rendered turn header; transcript rows never tick. */
export function WorkingHeading({ active, duration = '0s', startedAt }: { active: boolean; duration?: string; startedAt?: number }) {
  const [now, setNow] = useState(Date.now)
  useEffect(() => {
    if (!active) return
    setNow(Date.now())
    const timer = window.setInterval(() => setNow(Date.now()), 1000)
    return () => window.clearInterval(timer)
  }, [active, startedAt])
  const seconds = Math.max(0, Math.floor((now - (startedAt ?? now)) / 1000))
  const elapsed = startedAt == null ? duration : seconds < 60 ? `${seconds}s` : `${Math.floor(seconds / 60)}m ${seconds % 60}s`
  return <span>{active ? `Working ${elapsed}` : `Worked for ${duration}`}</span>
}

export const WorkSectionContext = createContext<{ expanded: boolean; toggle: () => void } | null>(null)

export interface WorkTraceDisclosureState {
  expanded?: boolean
  thoughts?: Map<number, boolean>
}

/** Share an explicit choice across live/saved rows; untouched turns use the setting. */
export function useWorkTraceDisclosure(disclosureState?: WorkTraceDisclosureState) {
  const defaultExpanded = useAppStore((state) => state.expandWorkTraces)
  const [override, setOverride] = useState<WorkTraceDisclosureState>({})
  const state = disclosureState ?? override
  const section = useContext(WorkSectionContext)
  const expanded = section?.expanded ?? state.expanded ?? defaultExpanded
  return { expanded, state, toggle: section?.toggle ?? (() => {
    if (disclosureState) disclosureState.expanded = !expanded
    setOverride({ ...state, expanded: !expanded })
  }) }
}

/** Thought order survives removal of permission events from saved history. */
export function useThoughtDisclosure(state?: WorkTraceDisclosureState, index = 0, defaultOpen = false) {
  const [local, setLocal] = useState({ open: defaultOpen })
  const open = state ? state.thoughts?.get(index) ?? defaultOpen : local.open
  return { open, toggle: () => {
    if (state) (state.thoughts ??= new Map()).set(index, !open)
    setLocal({ open: !open })
  } }
}

function ConversationThought({ text, disclosureState, thoughtIndex }: { text: string; disclosureState?: WorkTraceDisclosureState; thoughtIndex?: number }) {
  const { open, toggle } = useThoughtDisclosure(disclosureState, thoughtIndex)
  const preview = text.split('\n').find((line) => line.trim() && !/^\s*#{1,6}\s+(?:Reasoning|Thoughts)\s*$/i.test(line))?.replace(/^\s*#{1,6}\s+/, '') ?? ''
  return <li className="conversation-work__event conversation-work__note">
    <details className="conversation-work__thought" open={open}>
      <summary onClick={(event) => { event.preventDefault(); toggle() }}>
        <Brain size={15} className="conversation-work__event-icon" aria-hidden />
        <span className="conversation-work__thought-label">Thoughts</span>
        <span className="conversation-work__tool-title">{preview}</span>
        <ChevronRight size={12} className="conversation-work__chevron" aria-hidden />
      </summary>
      {open && <div className="conversation-work__reasoning"><MarkdownContent content={text} /></div>}
    </details>
  </li>
}

function ConversationTool({ tool, onSelectTool, renderSubAgentHistory }: {
  tool: AcpToolCall
  onSelectTool: (toolId: string) => void
  renderSubAgentHistory?: (tool: AcpToolCall) => ReactNode
}) {
  const [historyOpen, setHistoryOpen] = useSubAgentDisclosure(tool.id)
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
      <summary onClick={(event) => { event.preventDefault(); setHistoryOpen(!historyOpen) }}>Sub-agent transcript <ChevronRight size={12} aria-hidden /></summary>
      {historyOpen && <div className="conversation-work__history">{renderSubAgentHistory(tool)}</div>}
    </details>}
  </li>
}

/** Compact rows can be placed directly in the ordered transcript without grouping earlier text. */
export function ConversationWorkRow({ text, tool, onSelectTool, renderSubAgentHistory, disclosureState, thoughtIndex }: {
  disclosureState?: WorkTraceDisclosureState
  thoughtIndex?: number
  text?: string
  tool?: AcpToolCall
  onSelectTool: (toolId: string) => void
  renderSubAgentHistory?: (tool: AcpToolCall) => ReactNode
}) {
  if (!tool && !text?.trim()) return null
  return <ol className="conversation-work__timeline conversation-work__inline" aria-label={tool ? 'Tool activity' : 'Thoughts'}>
    {tool ? <ConversationTool tool={tool} onSelectTool={onSelectTool} renderSubAgentHistory={renderSubAgentHistory} /> : <ConversationThought text={text!} disclosureState={disclosureState} thoughtIndex={thoughtIndex} />}
  </ol>
}

/** Presentation only: tool detail and sub-agent callbacks remain owned by the chat. */
export function ConversationWork({ events, tools, active, duration, onSelectTool, renderSubAgentHistory, headerOnly = false, startedAt, expanded, onToggle, disclosureState, sectionHeading = false, collapsible = true }: {
  disclosureState?: WorkTraceDisclosureState
  expanded?: boolean
  onToggle?: () => void
  startedAt?: number
  headerOnly?: boolean
  sectionHeading?: boolean
  collapsible?: boolean
  events: AcpTurnEvent[]
  tools: AcpToolCall[]
  active: boolean
  duration: string
  onSelectTool: (toolId: string) => void
  renderSubAgentHistory?: (tool: AcpToolCall) => ReactNode
}) {
  const section = useContext(WorkSectionContext)
  const disclosure = useWorkTraceDisclosure(disclosureState)
  const open = expanded ?? disclosure.expanded
  const visibleOpen = active ? true : open
  const toggle = onToggle ?? disclosure.toggle
  if (headerOnly && section && !sectionHeading) return null
  if (headerOnly) return <div className="conversation-work" data-testid="working-summary" data-active={active}>
    {active ? (
      <span className="conversation-work__heading" aria-label="Work in progress">
        <WorkingHeading active={active} duration={duration} startedAt={startedAt} />
      </span>
    ) : collapsible ? (
      <button type="button" className="conversation-work__heading conversation-work__toggle" aria-expanded={open} aria-label={open ? 'Collapse work trace' : 'Expand work trace'} onClick={toggle}>
        <WorkingHeading active={active} duration={duration} startedAt={startedAt} />
        <ChevronRight size={14} className="conversation-work__chevron" aria-hidden />
      </button>
    ) : (
      <span className="conversation-work__heading">
        <WorkingHeading active={active} duration={duration} startedAt={startedAt} />
      </span>
    )}
    <span className="conversation-work__divider" aria-hidden />
  </div>
  if (events.length === 0 && tools.length === 0) return null

  const toolById = new Map(tools.map((tool) => [tool.id, tool]))
  const referencedTools = new Set(events.flatMap((event) => event.type === 'tool_call' ? [event.toolCallId] : []))
  let thoughtIndex = 0
  const Container = section ? 'div' : 'details'
  return <Container className="conversation-work" data-testid={section ? undefined : "working-summary"} data-active={active} {...(section ? {} : { open: visibleOpen })}>
    {!section && <summary className="conversation-work__summary" onClick={active ? undefined : (event) => { event.preventDefault(); toggle() }}>
      <span className="conversation-work__heading"><span><WorkingHeading active={active} duration={duration} startedAt={startedAt} /></span>{!active && <ChevronRight size={14} className="conversation-work__chevron" aria-hidden />}</span>
      <span className="conversation-work__divider" aria-hidden />
    </summary>}
    {visibleOpen && <div className="conversation-work__expanded">
      <ol className="conversation-work__timeline" aria-label="Work activity">
        {events.map((event, index) => {
          if (event.type === 'tool_call') {
            const tool = toolById.get(event.toolCallId) ?? {
              id: event.toolCallId, title: event.toolCallId, kind: 'tool', status: 'pending', content: '',
            }
            return <ConversationTool key={`tool-${tool.id}-${index}`} tool={tool} onSelectTool={onSelectTool} renderSubAgentHistory={renderSubAgentHistory} />
          }
          if (event.type === 'thought' && event.text.trim()) return <ConversationThought key={`thought-${index}`} text={event.text} disclosureState={disclosure.state} thoughtIndex={thoughtIndex++} />
          if (event.type === 'assistant_text' && event.text.trim()) {
            return <li key={`text-${index}`} className="conversation-work__event conversation-work__note">
              <MessageSquare size={15} className="conversation-work__event-icon" aria-hidden />
              <MarkdownContent content={event.text} />
            </li>
          }
          return null
        })}
        {tools.filter((tool) => !referencedTools.has(tool.id)).map((tool) => <ConversationTool key={`extra-${tool.id}`} tool={tool} onSelectTool={onSelectTool} renderSubAgentHistory={renderSubAgentHistory} />)}
      </ol>
      </div>}
  </Container>
}
