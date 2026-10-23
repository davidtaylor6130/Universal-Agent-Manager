import { clearChatLeaf, chatGridLeaves, readChatGridLayout, setChatInLeaf, writeChatGridLayout } from './chatGridStorage'
import type { Session } from '../types/session'

type ParentVisit = { parentId: string; providerId: string; hostId: string; parentCreatedAt: number; parentWorkspace?: string }
const visits = new Map<string, ParentVisit>()
const listeners = new Set<() => void>()
const storageKey = 'uam-subagent-navigation-v1'
let loaded = false
let revision = 0
const provider = (session: Session) => session.providerId || 'gemini-cli'
const host = (session: Session) => session.executionHostId || 'local'

const workspace = (session: Session) => {
  const path = (session.workspaceWorktreeDirectory?.trim() || session.workspaceDirectory?.trim() || '').replace(/\\/g, '/').replace(/\/+$/, '')
  return path ? /^[a-z]:/i.test(path) ? path.toLowerCase() : path : `folder:${session.folderId || ''}`
}

function parentVisit(child: Session): ParentVisit | undefined {
  loadVisits()
  // Any explicit lineage takes precedence, including an incomplete/invalid orphan link.
  if (child.subagentParentChatId || child.subagentParentCreatedAt) return {
    parentId: child.subagentParentChatId || '', parentCreatedAt: new Date(child.subagentParentCreatedAt || '').getTime(),
    providerId: provider(child), hostId: host(child), parentWorkspace: workspace(child),
  }
  return visits.get(child.id)
}

export const hasNativeSubagentLineage = (child: Session) => Boolean(parentVisit(child))

/** Existing native Session capability, independent of display-only lineage. */
export const canExecuteNativeSubagent = (child: Session) => !child.importedReadOnly && (child.providerId === 'codex-cli' || child.providerId === 'opencode-cli' || (child.providerId === 'antigravity-cli' && host(child) === 'local'))

function parentMatches(child: Session, parent: Session, visit: ParentVisit) {
  return parent.id !== child.id && parent.createdAt.getTime() === visit.parentCreatedAt &&
    provider(parent) === visit.providerId && provider(child) === visit.providerId &&
    host(parent) === visit.hostId && host(child) === visit.hostId && workspace(parent) === workspace(child) &&
    (visit.parentWorkspace === undefined || workspace(parent) === visit.parentWorkspace)
}

function lineageIsAcyclic(child: Session, byId: Map<string, Session>) {
  const seen = new Set<string>()
  let current: Session | undefined = child
  while (current) {
    if (seen.has(current.id)) return false
    seen.add(current.id)
    const link = parentVisit(current)
    current = link ? byId.get(link.parentId) : undefined
  }
  return true
}

function loadVisits() {
  if (loaded) return
  loaded = true
  try {
    const records: unknown = JSON.parse(localStorage.getItem(storageKey) || '[]')
    if (!Array.isArray(records)) return
    for (const record of records.slice(-256)) {
      if (!Array.isArray(record) || record.length !== 2 || typeof record[0] !== 'string' || !record[1] || typeof record[1] !== 'object') continue
      const value = record[1] as Partial<ParentVisit>
      if (typeof value.parentId === 'string' && typeof value.providerId === 'string' && typeof value.hostId === 'string' && typeof value.parentCreatedAt === 'number' && Number.isFinite(value.parentCreatedAt)) visits.set(record[0], value as ParentVisit)
    }
  } catch { /* Navigation context is optional; native history and selection remain authoritative. */ }
}

export const subscribeSubagentNavigation = (listener: () => void) => { listeners.add(listener); return () => { listeners.delete(listener) } }
export const subagentNavigationRevision = () => { loadVisits(); return revision }

/** A successful child-open receipt records navigation context, never native resume authority. */
export function rememberSubagentParent(parent: Session, childChatId: string) {
  loadVisits()
  if (!childChatId || parent.id === childChatId) return
  visits.delete(childChatId)
  visits.set(childChatId, { parentId: parent.id, providerId: provider(parent), hostId: host(parent), parentCreatedAt: parent.createdAt.getTime(), parentWorkspace: workspace(parent) })
  if (visits.size > 256) visits.delete(visits.keys().next().value!)
  try { localStorage.setItem(storageKey, JSON.stringify([...visits])) } catch { /* Keep this window's navigation usable when browser storage is unavailable. */ }
  revision++
  listeners.forEach(listener => listener())
}

export function subagentParentFor(child: Session, sessions: Session[]) {
  const visit = parentVisit(child)
  if (!visit) return undefined
  const parent = sessions.find(candidate => candidate.id === visit.parentId)
  if (!parent || !parentMatches(child, parent, visit)) return undefined
  const seen = new Set([child.id])
  let ancestor: Session | undefined = parent
  while (ancestor) {
    if (seen.has(ancestor.id)) return undefined
    seen.add(ancestor.id)
    const next = parentVisit(ancestor)
    ancestor = next ? sessions.find(candidate => candidate.id === next.parentId) : undefined
  }
  return parent
}

export type SubagentRoute = ParentVisit & ({ kind: 'managed'; runId: string; transcriptChatId?: string; title?: string; status?: string } | { kind: 'native'; chatId: string })
const routes = new Map<string, SubagentRoute>()
const changed = () => { revision++; listeners.forEach(listener => listener()) }

/** Virtual pane routes carry read-only identity, never a Session or execution authority. */
export function openSubagentView(parent: Session, child: { kind: 'managed'; runId: string; transcriptChatId?: string; title?: string; status?: string } | { kind: 'native'; chatId: string }, targetLeafId?: string) {
  const layout = readChatGridLayout()
  const leaves = chatGridLeaves(layout.root)
  const leaf = leaves.find(candidate => candidate.id === targetLeafId) || leaves.find(candidate => candidate.id === layout.activeLeafId && candidate.sessionId === parent.id) || leaves.find(candidate => candidate.sessionId === parent.id) || leaves.find(candidate => candidate.id === layout.activeLeafId) || leaves[0]
  if (!leaf || (child.kind === 'managed' ? !child.runId : !child.chatId || child.chatId === parent.id)) return
  let next = leaf.sessionId === parent.id ? { ...layout, activeLeafId: leaf.id } : setChatInLeaf(layout, parent.id, leaf.id)
  // Migrate a legacy native-child pane without leaving a second executable row/view.
  if (child.kind === 'native') for (const candidate of chatGridLeaves(next.root)) {
    if (candidate.id !== leaf.id && candidate.sessionId === child.chatId) next = clearChatLeaf(next, candidate.id)
  }
  next = { ...next, activeLeafId: leaf.id }
  routes.set(leaf.id, { ...child, parentId: parent.id, providerId: provider(parent), hostId: host(parent), parentCreatedAt: parent.createdAt.getTime(), parentWorkspace: workspace(parent) })
  writeChatGridLayout(next)
  changed()
}

export function subagentRouteFor(leafId: string, parent: Session): SubagentRoute | undefined {
  const route = routes.get(leafId)
  return route && route.parentId === parent.id && route.parentCreatedAt === parent.createdAt.getTime() && route.providerId === provider(parent) && route.hostId === host(parent) && (route.parentWorkspace === undefined || route.parentWorkspace === workspace(parent)) ? route : undefined
}

export function clearSubagentView(leafId: string) {
  if (routes.delete(leafId)) changed()
}

/** Explicit parent selection also returns when the selected session ID did not change. */
export function returnToSubagentParent(parentId: string, targetLeafId?: string) {
  const layout = readChatGridLayout()
  const leaves = chatGridLeaves(layout.root)
  const target = leaves.find(leaf => leaf.id === targetLeafId) || leaves.find(leaf => leaf.id === layout.activeLeafId && leaf.sessionId === parentId) || leaves.find(leaf => leaf.sessionId === parentId)
  if (target && routes.get(target.id)?.parentId === parentId) clearSubagentView(target.id)
}

export function activateSubagentLeaf(leafId: string) {
  const layout = readChatGridLayout()
  if (!chatGridLeaves(layout.root).some(leaf => leaf.id === leafId)) return
  writeChatGridLayout({ ...layout, activeLeafId: leafId })
  changed()
}

let cachedSessions: Session[] | undefined
let cachedRevision = -1
let childGroups = new Map<string, Session[]>()
const noChildren: Session[] = []
/** One metadata projection per immutable session list/navigation receipt, not per row/token. */
export function nativeSubagentChildren(parentId: string, sessions: Session[]): Session[] {
  if (cachedSessions !== sessions || cachedRevision !== revision) {
    cachedSessions = sessions
    cachedRevision = revision
    childGroups = new Map()
    const byId = new Map(sessions.map(session => [session.id, session]))
    for (const child of sessions) {
      const visit = parentVisit(child)
      const parent = visit ? byId.get(visit.parentId) : undefined
      if (!parent || !parentMatches(child, parent, visit!) || !lineageIsAcyclic(child, byId)) continue
      const children = childGroups.get(parent.id) || []
      children.push(child)
      childGroups.set(parent.id, children)
    }
  }
  return childGroups.get(parentId) || noChildren
}
