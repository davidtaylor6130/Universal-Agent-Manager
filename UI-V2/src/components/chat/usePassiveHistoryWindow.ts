import { useEffect, useLayoutEffect, useRef, useState, type RefObject } from 'react'
import { isCefContext, sendToCEF } from '../../ipc/cefBridge'
import { chatSearchPageRequest } from '../../utils/chatSearchResults'
import { sanitizeCppMessage } from '../../store/cpp/sanitizers'
import { buildMessageFromCpp } from '../../store/cpp/reconcile'
import type { ChatMessagesResponse } from '../../store/cpp/types'
import type { Message } from '../../types/message'

const WINDOW_ROWS = 300
const visits = new Map<string, { index: number; offset: number }>()
type Segment = { first: number; rows: Message[] }
type Alignment = { index: number; offset: number; content?: string; end?: boolean; start?: boolean }
type Direction = 'target' | 'older' | 'newer' | 'latest'
export type PassiveHistoryConfig = {
  enabled: boolean; owner: string; childId: string; digest: string; total: number
  messages: Message[]; first: number; current: () => boolean
}
export type PassiveHistoryView = {
  messages: Message[]; nativeStartIndex: number; start: number; scrollRef: RefObject<HTMLDivElement>
  onScroll: () => void; revealIndex: (index: number) => void; showLatest: () => void
  retryOlder: () => void; newContent: boolean; older: 'idle'; busy: boolean
  historyError: string; retryHistory?: () => void; awayFromLatest: boolean
}

/** Display-local passive read window. No store setters, global search, hydration or runtime controls. */
export function usePassiveHistoryWindow(config: PassiveHistoryConfig) {
  const [state, setState] = useState<{ owner: string; segment: Segment | null; start: number; busy: boolean; error: string }>(() => ({
    owner: config.owner, segment: null, start: Math.max(0, config.messages.length - 20), busy: false, error: '',
  }))
  const owned = state.owner === config.owner ? state : { owner: config.owner, segment: null, start: Math.max(0, config.messages.length - 20), busy: false, error: '' }
  const segment = owned.segment ?? { first: config.first, rows: config.messages }
  const scrollRef = useRef<HTMLDivElement>(null)
  const lastRegion = useRef<HTMLDivElement | null>(null)
  const generation = useRef(0)
  const mounted = useRef(false)
  const inFlight = useRef(false)
  const initialized = useRef(false)
  const following = useRef(true)
  const initialFill = useRef(false)
  const alignment = useRef<Alignment | null>(null)
  const retry = useRef<{ direction: Direction; index?: number } | null>(null)
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
    if (following.current && live.current.segment.first + live.current.segment.rows.length === owner.total) { visits.delete(owner.owner); return }
    if (!(scrollRef.current ?? lastRegion.current)?.isConnected) return
    const position = snapshot()
    if (!position || !Number.isSafeInteger(position.index)) return
    visits.delete(owner.owner)
    visits.set(owner.owner, { index: position.index, offset: position.offset })
    if (visits.size > 64) visits.delete(visits.keys().next().value!)
  }
  const read = async (direction: Direction, index?: number) => {
    const { config: owner, segment: previous } = live.current
    if (!owner.enabled || inFlight.current) return false
    const valid = () => mounted.current && live.current.config.owner === owner.owner && owner.current()
    if (!valid() || !isCefContext() || !owner.digest || owner.digest.length > 256 || !Number.isSafeInteger(owner.total) || owner.total <= 0) {
      setState(value => ({ ...value, error: 'This child catalogue is unavailable. Refresh the conversation before reading history.' }))
      return false
    }
    const end = previous.first + previous.rows.length
    const target = direction === 'latest' ? owner.total - 1 : index
    if ((direction === 'target' || direction === 'latest') && (!Number.isSafeInteger(target) || target! < 0 || target! >= owner.total)) return false
    const pin = direction === 'target' || direction === 'latest'
      ? { index: target!, offset: 0, start: direction === 'target', end: direction === 'latest' } : snapshot()
    if (!pin) {
      retry.current = { direction, index }
      setState(value => ({ ...value, error: 'The visible reading position is unavailable. Loaded messages are retained; choose a rail mark or retry history.' }))
      return false
    }
    if (direction === 'older' && previous.first <= 0 || direction === 'newer' && end >= owner.total) return false
    const before = direction === 'older' ? previous.first : Math.min(owner.total, end + 100)
    const limit = direction === 'older' ? Math.min(100, previous.first) : Math.min(100, owner.total - end)
    const payload = direction === 'target' || direction === 'latest'
      ? chatSearchPageRequest({ chatId: owner.childId, messageIndex: target!, messagesDigest: owner.digest, field: 'content', excerpt: '' })
      : { chatId: owner.childId, limit, before, deferToolCallContent: true }
    const serial = ++generation.current
    inFlight.current = true
    retry.current = { direction, index }
    setState(value => ({ ...value, owner: owner.owner, busy: true, error: '' }))
    try {
      const response = await sendToCEF<ChatMessagesResponse>({ action: 'getChatMessages', payload }, false)
      if (!valid() || serial !== generation.current) return false
      const data = response.data
      const bound = direction === 'target' || direction === 'latest' ? 50 : 100
      if (!response.ok || !data || data.unchanged || data.chatId !== owner.childId || data.messagesDigest !== owner.digest ||
          data.totalCount !== owner.total || !Number.isSafeInteger(data.startIndex) || data.startIndex! < 0 ||
          !Array.isArray(data.messages) || !data.messages.length || data.messages.length > bound ||
          data.startIndex! + data.messages.length > owner.total) throw new Error('The child history page was refused or changed.')
      if ((direction === 'target' || direction === 'latest') && (target! < data.startIndex! || target! >= data.startIndex! + data.messages.length) ||
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
      if ((direction === 'older' || direction === 'newer') && (pin.index < next.first || pin.index >= next.first + next.rows.length ||
          next.rows[pin.index - next.first]?.content !== pin.content)) throw new Error('This viewport cannot retain its reading anchor in a bounded page. Choose the rail mark again.')
      if (!valid() || serial !== generation.current) return false
      alignment.current = direction === 'latest' ? { index: target!, offset: 0, end: true } : pin
      retry.current = null
      following.current = direction === 'latest'
      initialFill.current = direction === 'target'
      setState({ owner: owner.owner, segment: next, start: 0, busy: false, error: '' })
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
  useEffect(() => {
    mounted.current = true
    const owner = config.owner
    const visit = config.enabled ? visits.get(owner) : undefined
    if (visit && config.current()) {
      void read('target', visit.index).then(ok => { if (ok && mounted.current && live.current.config.owner === owner) {
        alignment.current = { index: visit.index, offset: visit.offset }
        setState(value => ({ ...value }))
      } })
    }
    return () => { if (live.current.config.owner === owner) remember(); mounted.current = false; ++generation.current; inFlight.current = false }
  }, [config.owner, config.enabled])
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
    } else if (!initialized.current) { initialized.current = true; region.scrollTop = region.scrollHeight }
    remember()
    // One bounded fill for an unusually tall viewport, never a load-all/retry loop.
    if (initialFill.current && !inFlight.current && !owned.error && region.scrollHeight <= region.clientHeight) {
      initialFill.current = false
      if (segment.first + segment.rows.length < config.total) void read('newer')
    }
  })
  const revealIndex = (index: number) => {
    if (inFlight.current) return
    following.current = false
    const { segment: current } = live.current
    if (index >= current.first && index < current.first + current.rows.length) {
      retry.current = null
      alignment.current = { index, offset: 0, start: true }
      setState(value => ({ ...value, start: 0, error: '' }))
    } else void read('target', index)
  }
  const onScroll = () => {
    if (!config.enabled) return
    const region = scrollRef.current
    if (!region) return
    const wasFollowing = following.current
    following.current = live.current.segment.first + live.current.segment.rows.length === config.total && region.scrollHeight - region.scrollTop - region.clientHeight < 48
    if (following.current !== wasFollowing) setState(value => ({ ...value }))
    remember()
    if (inFlight.current || live.current.owned.error) return
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
  const retryHistory = () => { const pending = retry.current; if (pending) void read(pending.direction, pending.index) }
  const view: PassiveHistoryView | undefined = config.enabled ? {
    messages: segment.rows, nativeStartIndex: segment.first, start: owned.start, scrollRef, onScroll, revealIndex,
    showLatest: () => { void read('latest') }, retryOlder: retryHistory, newContent: false, older: 'idle',
    busy: owned.busy, historyError: owned.error, retryHistory: retry.current ? retryHistory : undefined,
    awayFromLatest: Boolean(segment.rows.length && (!following.current || segment.first + segment.rows.length < config.total)),
  } : undefined
  return { view, invalidate }
}
