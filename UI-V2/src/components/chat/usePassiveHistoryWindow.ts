import { useEffect, useLayoutEffect, useRef, useState, type RefObject } from 'react'
import { isCefContext, sendToCEF } from '../../ipc/cefBridge'
import { chatSearchPageRequest } from '../../utils/chatSearchResults'
import { sanitizeCppMessage } from '../../store/cpp/sanitizers'
import { buildMessageFromCpp } from '../../store/cpp/reconcile'
import type { ChatMessagesResponse } from '../../store/cpp/types'
import type { AcceptedHistoryPage } from '../../store/storeTypes'
import type { Message } from '../../types/message'

const WINDOW_ROWS = 300
type Visit = { pin: Alignment; prefix: number }
const visits = new Map<string, Visit>()
type Segment = { first: number; rows: Message[] }
type Alignment = { index: number; offset: number; content?: string; end?: boolean; start?: boolean }
type Direction = 'target' | 'older' | 'newer' | 'latest' | 'restore'
export type PassiveHistoryConfig = {
  enabled: boolean; owner: string; childId: string; digest: string; total: number
  messages: Message[]; first: number; current: () => boolean
  displayOwner?: string; accepted?: { owner: string; page: AcceptedHistoryPage }
  pageCurrent?: (page: AcceptedHistoryPage) => boolean
}
export type PassiveHistoryView = {
  messages: Message[]; nativeStartIndex: number; start: number; scrollRef: RefObject<HTMLDivElement>
  onScroll: () => void; revealIndex: (index: number) => void; showLatest: () => void
  retryOlder: () => void; newContent: boolean; older: 'idle'; busy: boolean
  historyError: string; retryHistory?: () => void; awayFromLatest: boolean
}

/** Display-local passive read window. No store setters, global search, hydration or runtime controls. */
export function usePassiveHistoryWindow(config: PassiveHistoryConfig) {
  const displayOwner = config.displayOwner ?? config.owner
  const [state, setState] = useState<{ owner: string; segment: Segment | null; start: number; busy: boolean; error: string; provenance: string }>(() => ({
    owner: displayOwner, segment: config.displayOwner ? { first: config.first, rows: config.messages } : null,
    start: Math.max(0, config.messages.length - 20), busy: false, error: '', provenance: config.displayOwner ? '' : JSON.stringify([config.owner, config.digest]),
  }))
  const ownedState = state.owner === displayOwner ? state : { owner: displayOwner, segment: null,
    start: Math.max(0, config.messages.length - 20), busy: false, error: '', provenance: '' }
  // Until this pane has committed readable rows, a superseding shared reader can still supply
  // display evidence. It does not acquire accepted-page provenance or bounded-read authority.
  const fallback = Boolean(config.displayOwner) && !ownedState.segment?.rows.length && config.messages.length > 0
  const owned = fallback ? { ...ownedState, start: Math.max(0, config.messages.length - 20) } : ownedState
  const segment = fallback ? { first: config.first, rows: config.messages } : owned.segment ?? { first: config.first, rows: config.messages }
  const scrollRef = useRef<HTMLDivElement>(null)
  const lastRegion = useRef<HTMLDivElement | null>(null)
  const generation = useRef(0)
  const mounted = useRef(false)
  const inFlight = useRef(false)
  const initialized = useRef(false)
  const following = useRef(true)
  const attestedTotal = useRef(config.total)
  const scrollRevision = useRef(0)
  const initialFill = useRef(false)
  const alignment = useRef<Alignment | null>(null)
  const retry = useRef<{ direction: Direction; index?: number; visit?: Visit } | null>(null)
  const committed = useRef<Visit | null>(null)
  const transition = useRef<{ owner: string; visit: Visit | null; following: boolean; scrollRevision: number } | null>(null)
  const acceptedPage = useRef<AcceptedHistoryPage | null>(null)
  const pagingDigest = (owner: PassiveHistoryConfig) => owner.displayOwner
    ? owner.accepted?.owner === owner.owner ? owner.accepted.page.messagesDigest : '' : owner.digest
  const provenance = (owner: PassiveHistoryConfig) => JSON.stringify([owner.owner, pagingDigest(owner)])
  const live = useRef({ config, owned, segment })
  live.current = { config, owned, segment }
  const snapshot = (): Alignment | null => {
    const region = scrollRef.current ?? lastRegion.current
    if (!region?.isConnected) return null
    const current = live.current.segment
    const viewport = region.getBoundingClientRect()
    const row = [...region.querySelectorAll<HTMLElement>('[data-message-index]')].find(element => {
      const box = element.getBoundingClientRect()
      return box.height > 0 && box.bottom > viewport.top && box.top < viewport.bottom
    })
    if (!row) return null
    const index = Number(row.dataset.messageIndex)
    if (!Number.isSafeInteger(index) || index < current.first || index >= current.first + current.rows.length) return null
    return { index, offset: row.getBoundingClientRect().top - viewport.top,
      content: current.rows[index - current.first]?.content }
  }
  const remember = () => {
    const { config: owner } = live.current
    if (!mounted.current || !owner.enabled || !owner.owner || !owner.current()) return
    const key = owner.displayOwner ?? owner.owner
    if (following.current && live.current.segment.first + live.current.segment.rows.length === attestedTotal.current) { visits.delete(key); committed.current = null; return }
    if (!(scrollRef.current ?? lastRegion.current)?.isConnected) return
    const position = snapshot()
    if (!position || !Number.isSafeInteger(position.index)) return
    const visit = { pin: position, prefix: live.current.segment.first + live.current.owned.start }
    committed.current = visit
    visits.delete(key)
    visits.set(key, visit)
    if (visits.size > 64) visits.delete(visits.keys().next().value!)
  }
  const read = async (direction: Direction, index?: number, visit?: Visit) => {
    const { config: owner, segment: previous } = live.current
    if (!owner.enabled || inFlight.current) return false
    const page = owner.accepted?.page, digest = pagingDigest(owner)
    const valid = () => mounted.current && live.current.config.owner === owner.owner && owner.current() &&
      (!owner.displayOwner || live.current.config.accepted?.page === page && owner.accepted?.owner === owner.owner &&
        Boolean(page && owner.pageCurrent?.(page)))
    if (!valid() || !isCefContext() || !digest || digest.length > 256 || !Number.isSafeInteger(owner.total) || owner.total <= 0) {
      setState(value => ({ ...value, error: 'This child catalogue is unavailable. Refresh the conversation before reading history.' }))
      return false
    }
    if ((direction === 'older' || direction === 'newer') && live.current.owned.provenance !== provenance(owner)) {
      setState(value => ({ ...value, error: 'Loaded messages are retained. Refresh or choose a rail mark before reading adjacent history.' }))
      return false
    }
    const end = previous.first + previous.rows.length
    const target = direction === 'latest' ? owner.total - 1 : index
    if ((direction === 'target' || direction === 'latest' || direction === 'restore') && (!Number.isSafeInteger(target) || target! < 0 || target! >= owner.total)) {
      if (direction === 'restore') {
        retry.current = null
        setState(value => ({ ...value, busy: false, error: 'The retained reading position is no longer in the current history. Loaded messages remain shown; choose an available rail mark or Show latest.' }))
      }
      return false
    }
    let pin = direction === 'restore' ? visit?.pin : direction === 'target' || direction === 'latest'
      ? { index: target!, offset: 0, start: direction === 'target', end: direction === 'latest' } : snapshot()
    let restoreVisit = visit
    let restoredFollowing = false
    const scrollAtRead = scrollRevision.current
    if (!pin) {
      retry.current = { direction, index, visit }
      setState(value => ({ ...value, error: 'The visible reading position is unavailable. Loaded messages are retained; choose a rail mark or retry history.' }))
      return false
    }
    if (direction === 'older' && previous.first <= 0 || direction === 'newer' && end >= owner.total) return false
    const before = direction === 'older' ? previous.first : Math.min(owner.total, end + 100)
    const limit = direction === 'older' ? Math.min(100, previous.first) : Math.min(100, owner.total - end)
    const payload = direction === 'target' || direction === 'latest' || direction === 'restore'
      ? chatSearchPageRequest({ chatId: owner.childId, messageIndex: target!, messagesDigest: digest, field: 'content', excerpt: '' })
      : { chatId: owner.childId, limit, before, deferToolCallContent: true }
    const serial = ++generation.current
    inFlight.current = true
    retry.current = { direction, index, visit }
    setState(value => ({ ...value, owner: owner.displayOwner ?? owner.owner, busy: true,
      error: value.provenance === provenance(owner) ? '' : value.error }))
    try {
      const response = await sendToCEF<ChatMessagesResponse>({ action: 'getChatMessages', payload }, false)
      if (!valid() || serial !== generation.current) return false
      const data = response.data
      const bound = direction === 'target' || direction === 'latest' || direction === 'restore' ? 50 : 100
      if (!response.ok || !data || data.unchanged || data.chatId !== owner.childId || data.messagesDigest !== digest ||
          data.totalCount !== owner.total || !Number.isSafeInteger(data.startIndex) || data.startIndex! < 0 ||
          !Array.isArray(data.messages) || !data.messages.length || data.messages.length > bound ||
          data.startIndex! + data.messages.length > owner.total) throw new Error('The child history page was refused or changed.')
      if ((direction === 'target' || direction === 'latest' || direction === 'restore') && (target! < data.startIndex! || target! >= data.startIndex! + data.messages.length) ||
          direction === 'older' && data.startIndex! + data.messages.length !== previous.first ||
          direction === 'newer' && data.startIndex !== end) throw new Error('The child history page was not contiguous with this reading window.')
      const rows = data.messages.map((row, offset) => {
        const decoded = sanitizeCppMessage(row)
        if (!decoded) throw new Error('The child history page contained an invalid row.')
        return buildMessageFromCpp(owner.childId, decoded, data.startIndex! + offset)
      })
      let next: Segment = { first: data.startIndex!, rows }
      if (direction === 'older') next = { first: data.startIndex!, rows: [...rows, ...previous.rows].slice(0, WINDOW_ROWS) }
      if (direction === 'newer') {
        const combined = [...previous.rows, ...rows]
        const trim = Math.max(0, combined.length - WINDOW_ROWS)
        next = { first: previous.first + trim, rows: combined.slice(trim) }
      }
      // Restoring a distant committed range is itself asynchronous. Scrolling while it is held
      // supersedes its captured anchor just as it supersedes a pending ordinary tail refresh.
      if (direction === 'restore' && scrollAtRead !== scrollRevision.current) {
        if (following.current && page) {
          next = { first: page.startIndex, rows: page.messages }
          pin = { index: owner.total - 1, offset: 0, end: true }
          restoredFollowing = true
        } else {
          const latest = committed.current
          if (!latest) throw new Error('The reading position changed while history loaded. Loaded messages are retained; choose a rail mark again.')
          restoreVisit = latest
          pin = latest.pin
          retry.current = { direction, index: pin.index, visit: latest }
        }
      }
      if ((direction === 'older' || direction === 'newer') && (pin.index < next.first || pin.index >= next.first + next.rows.length ||
          next.rows[pin.index - next.first]?.content !== pin.content)) throw new Error('This viewport cannot retain its reading anchor in a bounded page. Choose the rail mark again.')
      if (direction === 'restore' && (pin.index < next.first || pin.index >= next.first + next.rows.length ||
          pin.content !== undefined && next.rows[pin.index - next.first]?.content !== pin.content)) throw new Error('The retained reading position changed. Loaded messages remain available; choose a rail mark to read the changed history.')
      if (!valid() || serial !== generation.current) return false
      alignment.current = direction === 'latest' ? { index: target!, offset: 0, end: true } : pin
      retry.current = null
      following.current = direction === 'latest' || restoredFollowing
      attestedTotal.current = owner.total
      initialFill.current = direction === 'target'
      const start = restoredFollowing ? Math.max(0, next.rows.length - 20) : direction === 'restore' ? Math.max(0, Math.min(pin.index, restoreVisit!.prefix) - next.first) : 0
      setState({ owner: owner.displayOwner ?? owner.owner, segment: next, start, busy: false, error: '', provenance: provenance(owner) })
      return true
    } catch (error) {
      if (valid() && serial === generation.current) setState(value => ({ ...value, busy: false,
        error: error instanceof Error ? error.message : 'The child history could not be read.' }))
      return false
    } finally {
      if (serial === generation.current) { inFlight.current = false; if (valid()) setState(value => value.busy ? { ...value, busy: false } : value) }
    }
  }
  const invalidate = () => {
    ++generation.current; inFlight.current = false; retry.current = null; alignment.current = null
    setState(value => ({ ...value, busy: false, error: '' }))
  }
  useLayoutEffect(() => {
    ++generation.current; inFlight.current = false; retry.current = null; alignment.current = null
    const visit = committed.current ?? visits.get(displayOwner) ?? null
    committed.current = visit
    if (visit) following.current = false
    transition.current = { owner: config.owner, visit, following: visit ? false : following.current, scrollRevision: scrollRevision.current }
    acceptedPage.current = null
    setState(value => value.busy || value.error ? { ...value, busy: false, error: '' } : value)
  }, [config.owner, config.enabled])
  useLayoutEffect(() => {
    if (acceptedPage.current && !config.pageCurrent?.(acceptedPage.current)) {
      ++generation.current; inFlight.current = false; retry.current = null; alignment.current = null
      acceptedPage.current = null
      setState(value => value.busy ? { ...value, busy: false } : value)
    }
  })
  useEffect(() => {
    mounted.current = true
    if (!config.displayOwner) {
      const visit = visits.get(displayOwner)
      if (visit && config.current()) void read('restore', visit.pin.index, visit)
    }
    return () => { remember(); mounted.current = false; ++generation.current; inFlight.current = false }
  }, [displayOwner, config.enabled])
  useEffect(() => {
    const candidate = config.accepted
    if (!config.enabled || !config.displayOwner || !candidate || candidate.owner !== config.owner || !config.current() ||
        !config.pageCurrent?.(candidate.page) || candidate.page.totalCount !== config.total || acceptedPage.current === candidate.page) return
    ++generation.current; inFlight.current = false; retry.current = null; alignment.current = null
    acceptedPage.current = candidate.page
    const pending = transition.current
    const captured = pending?.owner === config.owner && pending.scrollRevision === scrollRevision.current
    const visit = captured ? pending.visit : committed.current
    const tail = { first: candidate.page.startIndex, rows: candidate.page.messages }
    const follow = captured ? pending.following : following.current
    attestedTotal.current = candidate.page.totalCount
    if (follow || !visit) {
      following.current = true
      alignment.current = { index: config.total - 1, offset: 0, end: true }
      setState({ owner: displayOwner, segment: tail, start: Math.max(0, tail.rows.length - 20),
        busy: false, error: '', provenance: provenance(config) })
    } else if (visit.pin.index >= tail.first && visit.pin.index < tail.first + tail.rows.length &&
        (visit.pin.content === undefined || tail.rows[visit.pin.index - tail.first]?.content === visit.pin.content)) {
      alignment.current = visit.pin
      setState({ owner: displayOwner, segment: tail, start: Math.max(0, Math.min(visit.pin.index, visit.prefix) - tail.first),
        busy: false, error: '', provenance: provenance(config) })
    } else void read('restore', visit.pin.index, visit)
    transition.current = null
  }, [config.owner, config.accepted])
  useLayoutEffect(() => {
    const region = scrollRef.current
    if (!config.enabled || !region) return
    lastRegion.current = region
    const pin = alignment.current
    if (pin) {
      const row = region.querySelector<HTMLElement>(`[data-message-index="${pin.index}"]`)
      if (row) {
        if (pin.end) region.scrollTop = region.scrollHeight
        else if (pin.start) row.scrollIntoView?.({ block: 'start' })
        else region.scrollTop += row.getBoundingClientRect().top - region.getBoundingClientRect().top - pin.offset
        alignment.current = null
      }
    } else if (!initialized.current) {
      initialized.current = true
      // A saved visit owns the first position. An automatic initial bottom scroll would emit a
      // browser scroll event and overwrite that intent while its bounded restoration is pending.
      if (!transition.current?.visit) region.scrollTop = region.scrollHeight
    }
    if (!inFlight.current && owned.provenance === provenance(config)) remember()
    // One bounded fill for an unusually tall viewport, never a load-all/retry loop.
    if (initialFill.current && !inFlight.current && owned.provenance === provenance(config) && !owned.error && region.scrollHeight <= region.clientHeight) {
      initialFill.current = false
      if (segment.first + segment.rows.length < config.total) void read('newer')
    }
  })
  const revealIndex = (index: number) => {
    if (inFlight.current) return
    following.current = false
    const { segment: current } = live.current
    if (live.current.owned.provenance === provenance(live.current.config) && index >= current.first && index < current.first + current.rows.length) {
      retry.current = null
      alignment.current = { index, offset: 0, start: true }
      setState(value => ({ ...value, start: 0, error: '' }))
    } else void read('target', index)
  }
  const onScroll = () => {
    if (!config.enabled) return
    const region = scrollRef.current
    if (!region) return
    ++scrollRevision.current
    const wasFollowing = following.current
    following.current = live.current.segment.first + live.current.segment.rows.length === attestedTotal.current && region.scrollHeight - region.scrollTop - region.clientHeight < 48
    if (following.current !== wasFollowing) setState(value => ({ ...value }))
    remember()
    if (inFlight.current || live.current.owned.error || live.current.owned.provenance !== provenance(live.current.config)) return
    if (region.scrollTop < 320) {
      if (live.current.owned.start > 0) {
        const pin = snapshot()
        if (!pin) {
          retry.current = null
          setState(value => ({ ...value, error: 'The visible reading position is unavailable. Loaded messages are retained; choose a rail mark again.' }))
          return
        }
        alignment.current = pin
        setState(value => ({ ...value, start: Math.max(0, value.start - 20) }))
      } else void read('older')
    } else if (region.scrollHeight - region.scrollTop - region.clientHeight < 320) void read('newer')
  }
  const retryHistory = () => { const pending = retry.current; if (pending) void read(pending.direction, pending.index, pending.visit) }
  const view: PassiveHistoryView | undefined = config.enabled ? {
    messages: segment.rows, nativeStartIndex: segment.first, start: owned.start, scrollRef, onScroll, revealIndex,
    showLatest: () => { void read('latest') }, retryOlder: retryHistory, newContent: false, older: 'idle',
    busy: owned.busy, historyError: owned.error, retryHistory: retry.current ? retryHistory : undefined,
    awayFromLatest: Boolean(segment.rows.length && (!following.current || segment.first + segment.rows.length < config.total)),
  } : undefined
  return { view, invalidate }
}
