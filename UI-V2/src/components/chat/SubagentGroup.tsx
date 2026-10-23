import { useEffect, useId, useMemo, useRef, useState, type ReactNode } from 'react'
import { ChevronRight, Users } from 'lucide-react'
import { useAppStore, type AcpToolCall } from '../../store/useAppStore'
import { Button, Notice } from '../ui'
import { ToolStatusIcon } from './StatusHelpers'
import { useSubAgentDisclosure } from './ConversationWork'
import './SubagentGroup.css'
import { openSubagentView } from '../../utils/subagentNavigation'
import { boundedSubagentSummary, isManagedSubagentTool, managedResultCard, managedElapsed, managedRunActive, managedRunLabel } from './managedSubagents'
import { SubagentCard } from './SubagentCard'
import { isCefContext } from '../../ipc/cefBridge'

type Child = NonNullable<AcpToolCall['subAgentChildren']>[number]
type ChildTool = AcpToolCall & { subAgentChildren?: Child[] }

export function subagentState(status = '') {
  const value = status.replace(/[_ -]/g, '').toLowerCase()
  if (['running', 'inprogress', 'pending', 'pendinginit', 'queued'].includes(value)) return { label: value === 'pendinginit' ? 'Starting' : value === 'queued' ? 'Queued' : value === 'pending' ? 'Pending' : 'Running', active: true }
  const labels: Record<string, string> = { completed: 'Completed', failed: 'Failed', error: 'Failed', errored: 'Failed', cancelled: 'Stopped', canceled: 'Stopped', stopped: 'Stopped', interrupted: 'Interrupted', notfound: 'Unavailable', shutdown: 'Stopped' }
  return { label: labels[value] || 'Status unavailable', active: false }
}

/** Native timing is child-local evidence; saved running events never create a live clock. */
function nativeElapsed(child: Child, live: boolean, now: number): string | undefined {
  if (!child.id) return undefined
  const state = subagentState(child.status)
  const valid = (value: number | undefined): value is number => typeof value === 'number' && Number.isSafeInteger(value) && value >= 0 && value <= 8640000000000000
  let duration: number | undefined
  if (state.active) {
    if (live && valid(child.startedAtEpochMs) && now >= child.startedAtEpochMs) duration = now - child.startedAtEpochMs
  } else if (state.label !== 'Status unavailable') {
    if (valid(child.durationMs)) duration = child.durationMs
    else if (valid(child.startedAtEpochMs) && valid(child.finishedAtEpochMs) && child.finishedAtEpochMs >= child.startedAtEpochMs) duration = child.finishedAtEpochMs - child.startedAtEpochMs
  }
  if (duration === undefined) return undefined
  const seconds = Math.floor(duration / 1000)
  return seconds < 60 ? `${seconds}s` : seconds < 3600 ? `${Math.floor(seconds / 60)}m ${seconds % 60}s` : `${Math.floor(seconds / 3600)}h ${Math.floor(seconds % 3600 / 60)}m`
}

export function subagentRows(tools: ChildTool[]) {
  const rows = new Map<string, { key: string; child: Child; tool: AcpToolCall; presentation?: ReturnType<typeof managedResultCard> }>()
  for (const tool of tools) {
    if (!tool.isSubAgent) continue
    const result = managedResultCard(tool)
    const children = tool.subAgentChildren?.length ? tool.subAgentChildren
      : [{ id: tool.subAgentId || '', title: tool.subAgentTitle, status: '' }]
    for (const [index, child] of children.entries()) {
      const key = child.id ? `${isManagedSubagentTool(tool) ? 'managed:' : ''}${child.id}` : `event:${tool.id}`
      const previous = rows.get(key)
      const presentation = result?.runId === child.id ? result : undefined
      rows.set(key, { key, presentation, child: { ...child, status: presentation?.status || child.status || '', title: child.title || previous?.child.title || tool.subAgentTitle || (isManagedSubagentTool(tool) ? presentation?.agentId || 'Managed subagent' : `${tool.title || 'Subagent'}${children.length > 1 ? ` ${index + 1}` : ''}`) }, tool })
    }
  }
  return [...rows.values()]
}

/** The parent turn owns the group; native identity and available control stay with its adapter. */
type GroupProps = {
  tools: AcpToolCall[]
  active?: boolean
  compactNative?: boolean
  sourceChatId?: string
  onSelectTool?: (toolId: string) => void
  renderHistory?: (tool: AcpToolCall) => ReactNode
}

export function SubagentGroup(props: GroupProps) {
  const rows = useMemo(() => subagentRows(props.tools), [props.tools])
  return rows.length ? <SubagentGroupContent key={`${props.sourceChatId ?? 'recorded'}:${props.tools.find(tool => tool.isSubAgent)?.id || ''}`} {...props} active={Boolean(props.sourceChatId && props.active)} rows={rows} /> : null
}

function SubagentGroupContent({ tools, sourceChatId, renderHistory, onSelectTool, rows, active = false, compactNative = false }: GroupProps & { rows: ReturnType<typeof subagentRows> }) {
  const [open, setOpen] = useSubAgentDisclosure(`children:${tools.find(tool => tool.isSubAgent)?.id || ''}`)
  const [opening, setOpening] = useState('')
  const groupRef = useRef<HTMLDetailsElement | null>(null)
  const openingRef = useRef(false)
  const [error, setError] = useState('')
  const openSubAgentSession = useAppStore(state => state.openSubAgentSession)
  const knownRunning = active ? rows.filter(row => row.child.id && (isManagedSubagentTool(row.tool) ? managedRunActive(row.child.status || '') : subagentState(row.child.status).active)).length : 0
  const childCount = rows.filter(row => row.child.id).length
  const eventCount = rows.length - childCount
  const [now, setNow] = useState(Date.now)
  useEffect(() => {
    if (!open || !active || !rows.some(row => row.presentation?.timing ? managedRunActive(row.child.status || '') : row.child.startedAtEpochMs !== undefined && subagentState(row.child.status).active)) return
    setNow(Date.now())
    const timer = window.setInterval(() => { if (document.visibilityState !== 'hidden') setNow(Date.now()) }, 1000)
    return () => window.clearInterval(timer)
  }, [open, active, rows])
  if (!rows.length) return null

  const openChild = async (child: Child, key: string, tool: AcpToolCall) => {
    if (!sourceChatId || !child.id || openingRef.current) return
    if (isManagedSubagentTool(tool)) {
      if (!isCefContext()) return
      const parent = useAppStore.getState().sessions.find(session => session.id === sourceChatId)
      if (parent) { useAppStore.getState().setActiveSession(parent.id); openSubagentView(parent, { kind: 'managed', runId: child.id, title: child.title }) }
      else setError('The parent chat is unavailable. Select it again to open this child.')
      return
    }
    openingRef.current = true
    const selectedBeforeOpen = useAppStore.getState().activeSessionId
    setOpening(key)
    setError('')
    try {
      if (!await openSubAgentSession(sourceChatId, child.id, child.title || '', true) && useAppStore.getState().activeSessionId === selectedBeforeOpen) setError('Could not open this subagent conversation. Try again.')
    } catch { if (useAppStore.getState().activeSessionId === selectedBeforeOpen) setError('Could not open this subagent conversation. Try again.') }
    finally { openingRef.current = false; setOpening('') }
  }


  return <details ref={groupRef} tabIndex={-1} className="uam-subagent-group uam-subagent-panel" open={open} onToggle={event => setOpen(event.currentTarget.open)}>
    <summary><Users size={15} aria-hidden /><span>{childCount} {childCount === 1 ? 'subagent' : 'subagents'}{eventCount ? `, ${eventCount} ${eventCount === 1 ? 'event' : 'events'}` : ''}</span>
      <span className="uam-subagent-group__meta" role="status" aria-live="polite">{knownRunning > 0 ? `${knownRunning} working` : ''}</span>
      <ChevronRight size={14} className="uam-subagent-group__chevron" aria-hidden />
    </summary>
    {open && <div className="uam-subagent-group__body">
      {error && <Notice tone="error" title="Conversation unavailable" dismissLabel="Dismiss conversation error">{error}</Notice>}
      <ul aria-label="Subagents">
        {rows.map(({ key, child, tool, presentation }) => <SubagentRow key={key} child={child} tool={tool} presentation={presentation} sourceChatId={sourceChatId} now={now} opening={opening} rowKey={key} active={active} compactNative={compactNative}
          disclosureKey={`${sourceChatId ?? 'recorded'}:${tools.find(tool => tool.isSubAgent)?.id || ''}:${key}`} openChild={openChild} renderHistory={sourceChatId ? renderHistory : undefined} onSelectTool={onSelectTool} />)}
      </ul>
    </div>}
  </details>
}

function SubagentRow({ child, tool, presentation, sourceChatId, now, opening, rowKey, disclosureKey, openChild, renderHistory, onSelectTool, active, compactNative }: {
  child: Child; tool: AcpToolCall; presentation?: ReturnType<typeof managedResultCard>; sourceChatId?: string; now: number; opening: string; rowKey: string; disclosureKey: string; active: boolean; compactNative: boolean
  openChild: (child: Child, key: string, tool: AcpToolCall) => Promise<void>
  renderHistory?: (tool: AcpToolCall) => ReactNode
  onSelectTool?: (toolId: string) => void
}) {
  const [preview, setPreview] = useSubAgentDisclosure(`preview:${disclosureKey}`)
  const previewId = useId()
  const managed = isManagedSubagentTool(tool)
  const childState = managed ? { label: child.status ? managedRunLabel(child.status) : 'Result received', active: managedRunActive(child.status || '') } : subagentState(child.status)
  const toolState = subagentState(tool.status)
  const title = child.title || 'Subagent'
  const providerId = useAppStore(state => {
    if (managed) return presentation?.providerId
    if (!sourceChatId) return undefined
    const native = state.sessions.find(session => session.id === child.id)
    return native ? native.providerId : state.sessions.find(session => session.id === sourceChatId)?.providerId
  })
  // A shared multi-child tool result cannot be attributed to one completed child.
  const content = !tool.contentDeferred && typeof tool.content === 'string' ? tool.content : ''
  const nativeResult = !managed && (tool.subAgentChildren?.length ?? 1) <= 1 && !/^[{[]/.test(content.trimStart()) ? content : ''
  const summary = managed ? presentation?.summary : boundedSubagentSummary(tool.currentActivity && (!child.id || childState.active) ? tool.currentActivity : nativeResult)
  const disconnected = managed && !isCefContext()
  return <li>
    <div className="uam-subagent-group__row">
      {compactNative && !managed ? <Button variant="ghost" size="sm" className="w-full min-w-0 justify-start" leadingIcon={<Users size={13} aria-hidden />}
        data-subagent-row={rowKey} disabled={!sourceChatId || !child.id || Boolean(opening)} aria-busy={opening === rowKey || undefined}
        onClick={() => { void openChild(child, rowKey, tool) }} aria-label={`Open ${child.title}`} aria-describedby={`${previewId}-status ${previewId}-timing`}>
        <span className="truncate">{title}</span>
        <span id={`${previewId}-status`} className="uam-subagent-group__meta">{child.id ? `${!active && childState.active ? 'Last reported: ' : ''}${childState.label}` : toolState.label}</span>
        <span id={`${previewId}-timing`} className="uam-subagent-group__meta">{nativeElapsed(child, active, now)}</span>
      </Button> : <SubagentCard data-subagent-row={rowKey} title={title} providerId={providerId} summary={summary} status={child.id ? `${!active && childState.active ? 'Last reported: ' : ''}${childState.label}` : toolState.label}
        elapsed={managed ? presentation?.timing && (active || !childState.active) ? managedElapsed({ ...presentation.timing, status: child.status || presentation.status }, now) : undefined : nativeElapsed(child, active, now)} working={Boolean(child.id) && active && childState.active} disabled={!sourceChatId || !child.id || Boolean(opening) || disconnected} aria-busy={opening === rowKey || undefined}
        onClick={() => { void openChild(child, rowKey, tool) }} aria-label={`Open ${child.title}`} />}
      {!child.id && ['Failed', 'Interrupted', 'Stopped'].includes(toolState.label) && <ToolStatusIcon status={tool.status} />}
    </div>
    {(onSelectTool || (renderHistory && child.id && !managed)) && <div className="uam-subagent-group__actions">
      {onSelectTool && <Button variant="ghost" size="sm" aria-label={`Details for ${title}`} onClick={() => onSelectTool(tool.id)}>Details</Button>}
      {renderHistory && child.id && !managed && <Button variant="ghost" size="sm" aria-label={`Preview ${title}`} aria-controls={previewId} aria-expanded={preview} onClick={() => setPreview(!preview)}>Preview</Button>}
    </div>}
    {!child.id && tool.currentActivity && <p className="uam-subagent-group__meta">{tool.currentActivity}</p>}
    {!child.id && ['Failed', 'Interrupted', 'Stopped'].includes(toolState.label) && content && <p className="uam-subagent-group__meta">{content.slice(0, 240)}</p>}
    {!child.id && <p className="uam-subagent-group__meta">The provider did not expose a separate conversation.</p>}
    {disconnected && <p className="uam-subagent-group__meta">Connect to UAM to open this managed conversation.</p>}
    {preview && <div id={previewId}>{renderHistory?.({ ...tool, status: childState.label !== 'Status unavailable' ? childState.active ? 'running' : 'completed' : tool.status, subAgentId: child.id, subAgentTitle: child.title })}</div>}
  </li>
}
