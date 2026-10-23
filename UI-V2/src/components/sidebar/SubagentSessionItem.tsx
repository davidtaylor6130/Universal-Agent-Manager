import type { Session } from '../../types/session'
import { createContext, memo, useContext, useEffect, useId, useMemo, useRef, useState, useSyncExternalStore, type ComponentProps } from 'react'
import { Bot, ChevronRight } from 'lucide-react'
import { useShallow } from 'zustand/react/shallow'
import { useAppStore } from '../../store/useAppStore'
import { nativeSubagentChildren, subscribeSubagentNavigation, subagentNavigationRevision } from '../../utils/subagentNavigation'
import { ManagedSubagentPanel } from '../chat/ManagedSubagentPanel'
import { SessionItem } from './SessionItem'

const hasManagedChildren = (session: Session) => (session.managedSubagentCount ?? 0) > 0 || (session.managedSubagentRevision ?? 0) > 0

/** The sidebar list scrolls inside its own container, so IntersectionObserver needs it as the root for the margin to apply. */
function scrollParent(node: HTMLElement | null): Element | null {
  for (let element = node?.parentElement; element; element = element.parentElement) {
    const overflow = getComputedStyle(element).overflowY
    if (overflow === 'auto' || overflow === 'scroll') return element
  }
  return null
}

/** Opening the app never reads every parent's managed ledger: `near` mounts a reader once its row nears the viewport; `visible` pauses it again off-screen. */
function useNearViewport(enabled: boolean) {
  const observable = typeof IntersectionObserver !== 'undefined'
  const [state, setState] = useState({ near: !observable, visible: !observable })
  const anchor = useRef<HTMLDivElement>(null)
  useEffect(() => {
    if (!enabled || !observable || !anchor.current) return
    const observer = new IntersectionObserver(entries => {
      const inView = entries[entries.length - 1]?.isIntersecting === true
      setState(previous => previous.visible === inView && (previous.near || !inView) ? previous : { near: previous.near || inView, visible: inView })
    }, { root: scrollParent(anchor.current), rootMargin: '240px' })
    observer.observe(anchor.current)
    return () => observer.disconnect()
  }, [enabled, observable])
  return { ...state, anchor }
}

/** Native subagent sessions matching the active search; a parent holding one opens, whatever its own choice. */
export const SubagentSearchMatches = createContext<ReadonlySet<string>>(new Set())

/** Native lineage is validated acyclic, so this walk terminates. */
function hasDescendant(ownerId: string, sessions: Session[], test: (id: string) => boolean): boolean {
  return nativeSubagentChildren(ownerId, sessions).some(child => test(child.id) || hasDescendant(child.id, sessions, test))
}

/** Each family member keeps its own native lineage and managed control authority. */
function OwnerChildren({ owner, grouped = false }: { owner: Session; grouped?: boolean }) {
  const sessions = useAppStore(state => state.sessions)
  const activeId = useAppStore(state => state.activeSessionId)
  const children = useMemo(() => nativeSubagentChildren(owner.id, sessions), [owner.id, sessions])
  const matches = useContext(SubagentSearchMatches)
  const searchOpen = useMemo(() => matches.size > 0 && hasDescendant(owner.id, sessions, id => matches.has(id)), [owner.id, sessions, matches])
  const activeInside = useMemo(() => hasDescendant(owner.id, sessions, id => id === activeId), [owner.id, sessions, activeId])
  const managed = hasManagedChildren(owner)
  // Closed by default and chosen per parent. Selecting a native descendant opens its parent once; collapsing afterwards stays collapsed.
  const [chosen, setChosen] = useState(activeInside)
  useEffect(() => { if (activeInside) setChosen(true) }, [activeInside, activeId])
  // Search shows a matching child through its parents without touching their choice or any unrelated managed ledger.
  const expanded = chosen || searchOpen
  const listId = useId()
  // Only a deliberate choice mounts a managed reader; a search match alone shows native rows and never reads a ledger.
  const { near, visible, anchor } = useNearViewport(managed && chosen)
  const [mountedReader, setMountedReader] = useState(false)
  useEffect(() => { if (chosen && near) setMountedReader(true) }, [chosen, near])
  const total = children.length + (owner.managedSubagentCount ?? 0)
  const hasList = children.length > 0 || managed
  const content = <>
    {grouped && <SessionItem sessionId={owner.id} session={owner} familySessionIds={[owner.id]} />}
    {hasList && <button type="button" className="uam-subagent-toggle" aria-expanded={expanded} aria-controls={listId} disabled={searchOpen} onClick={() => setChosen(open => !open)}>
      <ChevronRight size={12} className="uam-subagent-toggle__chevron" aria-hidden />
      <span>Subagents{total > 0 ? ` · ${total}` : ''}</span>
    </button>}
    <div id={listId} hidden={!expanded}>
      {expanded && children.map(child => <SubagentSessionItem key={child.id} sessionId={child.id} session={child} familySessionIds={[child.id]} subagentChild />)}
      {managed && <div ref={anchor} hidden={!chosen}>{near && (chosen || mountedReader) && <ManagedSubagentPanel sourceChatId={owner.id} catalogue viewportVisible={visible} collapsed={!chosen} />}</div>}
    </div>
  </>
  // The labelled group only exists once there is something to list.
  const hasRows = grouped || hasList
  return <div role={hasRows ? 'group' : undefined} aria-label={hasRows ? `Subagents of ${owner.name || 'chat'}` : undefined} data-subagent-parent={grouped ? owner.id : undefined}>{content}</div>
}

/** Children are one-line robot rows under their parent behind one per-parent disclosure, closed by default. */
export const SubagentSessionItem = memo(function SubagentSessionItem({ orphan = false, subagentChild = false, ...props }: ComponentProps<typeof SessionItem> & { orphan?: boolean; subagentChild?: boolean }) {
  useSyncExternalStore(subscribeSubagentNavigation, subagentNavigationRevision)
  const owners = useAppStore(useShallow(state => {
    const root = props.session ?? state.sessions.find(session => session.id === props.sessionId)
    const family = root ? [root] : []
    for (const id of props.familySessionIds ?? []) {
      if (id === props.sessionId) continue
      const member = state.sessions.find(session => session.id === id)
      if (member) family.push(member)
    }
    return family.filter(owner => hasManagedChildren(owner) || nativeSubagentChildren(owner.id, state.sessions).length > 0)
  }))
  const row = orphan && props.session ? <OrphanSessionRow child={props.session} /> : <SessionItem {...props} subagentRow={subagentChild} />
  if (!owners.length) return row
  return <div data-subagent-parent={props.sessionId}>
    {row}
    <div className="uam-subagent-catalogue__children">
      {owners.map(owner => <OwnerChildren key={owner.id} owner={owner} grouped={owner.id !== props.sessionId} />)}
    </div>
  </div>
})

export function OrphanSubagentItem({ child }: { child: Session }) {
  return <SubagentSessionItem sessionId={child.id} session={child} familySessionIds={[child.id]} orphan />
}

function OrphanSessionRow({ child }: { child: Session }) {
  const active = useAppStore(state => state.activeSessionId === child.id)
  return <button type="button" className={`uam-sb-row mx-1 flex min-h-[26px] w-[calc(100%-8px)] cursor-pointer items-center gap-1.5 rounded-md px-2.5 py-1 text-left text-xs${active ? ' uam-sb-row--selected' : ''}`} aria-current={active ? 'page' : undefined}
    onClick={() => useAppStore.getState().setActiveSession(child.id)} aria-label={`Open read-only subagent ${child.name}`}>
    <Bot size={16} className="shrink-0" aria-hidden />
    <span className="truncate">{child.name}</span>
  </button>
}
