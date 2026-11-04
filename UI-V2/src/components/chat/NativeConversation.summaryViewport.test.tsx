import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterAll, afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { NativeConversation } from './SubagentPaneContent'
import { useAppStore } from '../../store/useAppStore'
import type { Session } from '../../types/session'
import { fallbackProviderForId } from '../../utils/providerMetadata'
import { buildMessageFromCpp } from '../../store/cpp/reconcile'
import { subagentParentFor } from '../../utils/subagentNavigation'

// Register the REAL store's existing synchronous uamPush receiver at module import.
// No production seam or substitute state-patch reducer is introduced.
const bootstrap = vi.hoisted(() => {
    const cefQuery = window.cefQuery
    const uamPush = window.uamPush
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify({ ok: false, error: 'Test has no bootstrap state.' }))
    return { cefQuery, uamPush }
})

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
type Envelope = { action: string; requestId?: string; payload?: Record<string, unknown> }
type Held = { envelope: Envelope; done: boolean; finish: (ok?: boolean) => void }
type Visit = { index: number; text: string; offset: number; top: number }
let serial = 0
let previous: ReturnType<typeof useAppStore.getState>, previousCef: Window['cefQuery']
let host: HTMLDivElement, root: Root, parent: Session, child: Session
let envelopes: Envelope[], held: Held[], holdChild: boolean, editLast: boolean, revision: 'before' | 'after', total: number
let frames: Map<number, FrameRequestCallback>, frameId: number
const indices = (count: number) => Array.from({ length: Math.ceil(count / 2) }, (_, index) => index * 2)
const wire = (start: number, end: number) => Array.from({ length: end - start }, (_, offset) => {
    const index = start + offset
    return { role: index % 2 ? 'assistant' as const : 'user' as const,
        content: revision === 'after' && editLast && index === 79 ? 'UPDATED NATIVE LAST ROW 79' : `Native summary history row ${index}`,
        createdAt: new Date(Date.parse('2026-10-22T10:00:00Z') + index * 1000).toISOString() }
})
const region = () => host.querySelector<HTMLElement>('[aria-label="Native conversation messages"]')!
const rows = () => [...region().querySelectorAll<HTMLElement>('[data-message-index]')]
const snapshotVisit = (): Visit => {
    const bounds = region().getBoundingClientRect()
    const visible = rows().find(row => row.getBoundingClientRect().bottom > bounds.top && row.getBoundingClientRect().top < bounds.bottom)
    expect(visible, 'Geometry must establish an actual visible child row').toBeDefined()
    return { index: Number(visible!.dataset.messageIndex), text: visible!.textContent!,
        offset: visible!.getBoundingClientRect().top - bounds.top, top: region().scrollTop }
}
const flushFrames = async () => {
    // Bounded layout settling only; never advance provider/runtime/interval timers.
    for (let pass = 0; frames.size && pass < 20; ++pass) {
        const pending = [...frames.values()]; frames.clear()
        await act(async () => pending.forEach(callback => callback(pass * 16)))
    }
    expect(frames.size, 'The geometry fixture must settle within its finite frame budget').toBe(0)
}

beforeEach(() => {
    previous = useAppStore.getState(); previousCef = window.cefQuery; serial += 1
    const createdAt = new Date('2026-10-22T09:00:00Z')
    parent = { id: `summary-parent-${serial}`, name: 'Summary parent', providerId: 'codex-cli', folderId: null,
        viewMode: 'chat', createdAt, updatedAt: createdAt, executionHostId: 'local', workspaceDirectory: '/workspace/summary',
        workspaceWorktreeDirectory: '', messagesDigest: 'parent-summary', messageCount: 2, userMessageIndices: [0] }
    child = { ...parent, id: `summary-child-${serial}`, name: 'Passive summary child', importedReadOnly: true,
        createdAt: new Date('2026-10-22T09:01:00Z'), subagentParentChatId: parent.id,
        subagentParentCreatedAt: createdAt.toISOString(), messagesDigest: 'child-summary-before', messageCount: 80,
        userMessageIndices: indices(80) }
    envelopes = []; held = []; holdChild = false; editLast = false; revision = 'before'; total = 80; frames = new Map(); frameId = 0
    const storage = new Map<string, string>()
    vi.stubGlobal('localStorage', { getItem: (key: string) => storage.get(key) ?? null,
        setItem: (key: string, value: string) => storage.set(key, value), removeItem: (key: string) => storage.delete(key) })
    vi.stubGlobal('requestAnimationFrame', (callback: FrameRequestCallback) => { frames.set(++frameId, callback); return frameId })
    vi.stubGlobal('cancelAnimationFrame', (id: number) => frames.delete(id))
    vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
    vi.stubGlobal('IntersectionObserver', class { observe() {} unobserve() {} disconnect() {} })
    window.cefQuery = ({ request, onSuccess }) => {
        const envelope = JSON.parse(request) as Envelope
        envelopes.push(envelope)
        if (envelope.action === 'getChatMessages' && envelope.payload?.chatId === child.id) {
            const capturedTotal = total, capturedRevision = revision
            const pending: Held = { envelope, done: false, finish: (ok = true) => {
                if (pending.done) return
                pending.done = true
                const currentRevision = revision; revision = capturedRevision
                const startIndex = Math.max(0, capturedTotal - 50)
                const messages = wire(startIndex, capturedTotal); revision = currentRevision
                onSuccess(JSON.stringify(ok ? { ok: true, data: { chatId: child.id, startIndex, totalCount: capturedTotal,
                    messagesDigest: `child-page-${capturedRevision}`, userMessageIndices: indices(capturedTotal), messages } }
                    : { ok: false, error: 'Test cleanup retires held child history.' }))
            } }
            held.push(pending)
            if (!holdChild) pending.finish()
        } else onSuccess(JSON.stringify({ ok: envelope.action === 'releaseChatMessages', data: {} }))
    }
    useAppStore.setState({ sessions: [parent, child], providers: [fallbackProviderForId('codex-cli')], folders: [], resourceCollections: [],
        activeSessionId: parent.id, lastAppliedStateRevision: 1,
        messages: { [parent.id]: wire(0, 2).map((row, index) => buildMessageFromCpp(parent.id, row, index)),
            [child.id]: wire(30, 80).map((row, offset) => buildMessageFromCpp(child.id, row, 30 + offset)) },
        historyStartIndexBySessionId: { [parent.id]: 0, [child.id]: 30 }, chatHistoryErrorBySessionId: {}, chatSearchNavigation: null,
        acpBindingBySessionId: {}, cliBindingBySessionId: {}, markdownStoreAttachedBySessionId: {},
        sendAcpPrompt: vi.fn().mockResolvedValue(true), startAcpSession: vi.fn().mockResolvedValue(true),
        stopAcpSession: vi.fn().mockResolvedValue(true), retryFailedMessage: vi.fn().mockResolvedValue({ ok: false }),
        resumeQueuedAcpPrompts: vi.fn().mockResolvedValue(false) })
    host = document.createElement('div'); document.body.append(host); root = createRoot(host)
    const positions = new WeakMap<Element, number>()
    const isRegion = (element: Element) => element.classList.contains('uam-subagent-transcript')
    const regionRows = (element: Element) => [...element.querySelectorAll('[data-message-index]')]
    const rect = (top: number, height: number) => ({ x: 0, y: top, top, bottom: top + height, left: 0, right: 400,
        width: 400, height, toJSON: () => ({}) }) as DOMRect
    vi.spyOn(Element.prototype, 'scrollHeight', 'get').mockImplementation(function (this: Element) { return isRegion(this) ? regionRows(this).length * 40 : 0 })
    vi.spyOn(Element.prototype, 'clientHeight', 'get').mockImplementation(function (this: Element) { return isRegion(this) ? 300 : 0 })
    vi.spyOn(Element.prototype, 'scrollTop', 'get').mockImplementation(function (this: Element) { return positions.get(this) ?? 0 })
    vi.spyOn(Element.prototype, 'scrollTop', 'set').mockImplementation(function (this: Element, value: number) {
        positions.set(this, Math.max(0, Math.min(value, Math.max(0, this.scrollHeight - this.clientHeight))))
    })
    vi.spyOn(Element.prototype, 'getBoundingClientRect').mockImplementation(function (this: Element) {
        if (isRegion(this)) return rect(100, 300)
        const owner = this.closest('.uam-subagent-transcript')
        return owner && this.hasAttribute('data-message-index')
            ? rect(100 + regionRows(owner).indexOf(this) * 40 - owner.scrollTop, 40) : rect(0, 0)
    })
})

afterEach(async () => {
    try {
        await act(async () => { root.unmount(); held.forEach(pending => pending.finish(false)) })
        await act(async () => previous.unloadSessionMessages(child.id))
        expect(held.every(pending => pending.done)).toBe(true)
    } finally {
        window.cefQuery = previousCef; useAppStore.setState(previous, true); host.remove(); frames.clear()
        vi.unstubAllGlobals(); vi.restoreAllMocks()
    }
})
afterAll(() => {
    window.cefQuery = bootstrap.cefQuery
    if (bootstrap.uamPush) window.uamPush = bootstrap.uamPush
    else Reflect.deleteProperty(window, 'uamPush')
})

describe('actual NativeConversation summary-driven viewport, authored UNRUN', () => {
    it.each([
        { change: 'append', followingTail: false },
        { change: 'append', followingTail: true },
        { change: 'same-count edit', followingTail: false },
        { change: 'same-count edit', followingTail: true },
    ] as const)('reads a summary-only $change while preserving child reading intent (followingTail=$followingTail)', async ({ change, followingTail }) => {
        expect(useAppStore.getState().loadSessionMessages).toBe(previous.loadSessionMessages)
        expect(vi.isMockFunction(useAppStore.getState().loadSessionMessages)).toBe(false)
        expect(typeof window.uamPush, 'The actual CEF store must own its registered push receiver').toBe('function')
        expect(subagentParentFor(child, useAppStore.getState().sessions)?.id).toBe(parent.id)
        const parentMessages = useAppStore.getState().messages[parent.id]
        const parentSession = useAppStore.getState().sessions.find(session => session.id === parent.id)
        await act(async () => root.render(<NativeConversation parent={parent} chatId={child.id}
            navigationKey={`summary-pane-${serial}`} autoFocus={false} onBack={() => {}} />))
        await flushFrames()
        expect(held).toHaveLength(1); expect(held[0].done).toBe(true)
        expect(held[0].envelope.payload).toEqual({ chatId: child.id, limit: 50, deferToolCallContent: true, messagesDigest: '' })
        expect(rows().some(row => row.dataset.messageIndex === '79')).toBe(true)
        region().scrollTop = followingTail ? region().scrollHeight - region().clientHeight : 230
        await act(async () => region().dispatchEvent(new Event('scroll', { bubbles: true })))
        await flushFrames()
        const visit = snapshotVisit()
        if (!followingTail) expect(region().scrollTop).toBeLessThan(region().scrollHeight - region().clientHeight - 100)
        const assertParent = () => {
            const state = useAppStore.getState()
            expect(state.activeSessionId).toBe(parent.id); expect(state.chatSearchNavigation).toBeNull()
            expect(state.messages[parent.id]).toBe(parentMessages)
            expect(state.sessions.find(session => session.id === parent.id)).toEqual(parentSession)
            expect(state.historyStartIndexBySessionId[parent.id]).toBe(0)
        }
        holdChild = true; editLast = change === 'same-count edit'; revision = 'after'; total = change === 'append' ? 81 : 80
        // Actual native-style statePatch: no message array is injected into the store.
        const patch = { id: child.id, title: child.name, folderId: child.folderId, providerId: child.providerId,
            createdAt: child.createdAt.toISOString(), updatedAt: '2026-10-22T10:02:00Z',
            executionHostId: child.executionHostId, workspaceDirectory: child.workspaceDirectory,
            workspaceWorktreeDirectory: child.workspaceWorktreeDirectory, importedReadOnly: true,
            subagentParentChatId: parent.id, subagentParentCreatedAt: parent.createdAt.toISOString(),
            messageCount: total, messagesDigest: 'child-summary-after' }
        await act(async () => window.uamPush!({ type: 'statePatch', data: { stateRevision: 2, chats: [patch] } }))
        await flushFrames()
        expect(held, 'The changed summary must initiate exactly one new child-scoped bounded read').toHaveLength(2)
        expect(held[1].done).toBe(false)
        expect(held[1].envelope.payload).toEqual({ chatId: child.id, limit: 50, deferToolCallContent: true, messagesDigest: 'child-page-before' })
        expect(useAppStore.getState().sessions.find(session => session.id === child.id)?.messagesDigest).toBe('child-summary-after')
        expect(host.textContent).toContain('Native summary history row 79')
        assertParent()
        const pendingVisit = snapshotVisit()
        await act(async () => held[1].finish())
        await flushFrames()
        assertParent()
        expect(host.textContent).toContain(change === 'same-count edit' ? 'UPDATED NATIVE LAST ROW 79' : 'Native summary history row 80')
        if (change === 'append') expect(rows().some(row => row.dataset.messageIndex === '80')).toBe(true)
        const absolute = rows().map(row => Number(row.dataset.messageIndex))
        expect(absolute).toEqual(absolute.map((_, offset) => absolute[0] + offset))
        if (followingTail) expect(region().scrollTop).toBe(Math.max(0, region().scrollHeight - region().clientHeight))
        else {
            expect.soft(pendingVisit).toEqual(visit)
            expect.soft(snapshotVisit()).toEqual(visit)
        }
        // Repeated identical summary is observation, not another read or mutation.
        await act(async () => window.uamPush!({ type: 'statePatch', data: { stateRevision: 3, chats: [patch] } }))
        await flushFrames()
        expect(held).toHaveLength(2); assertParent()
        expect(host.querySelector('textarea, input, form')).toBeNull()
        const state = useAppStore.getState()
        expect(state.sendAcpPrompt).not.toHaveBeenCalled(); expect(state.startAcpSession).not.toHaveBeenCalled()
        expect(state.stopAcpSession).not.toHaveBeenCalled(); expect(state.retryFailedMessage).not.toHaveBeenCalled()
        expect(state.resumeQueuedAcpPrompts).not.toHaveBeenCalled()
        expect(envelopes.filter(item => item.action !== 'getChatMessages' && item.action !== 'releaseChatMessages')).toEqual([])
        expect(envelopes.filter(item => item.payload?.chatId === parent.id || item.payload?.refreshNative)).toEqual([])
    })
})

// Additive TESTONLY controls: intent may change after the summary read starts.
async function beginPendingAppendFromOffTail() {
    expect(useAppStore.getState().loadSessionMessages).toBe(previous.loadSessionMessages)
    expect(vi.isMockFunction(useAppStore.getState().loadSessionMessages)).toBe(false)
    expect(typeof window.uamPush).toBe('function')
    expect(subagentParentFor(child, useAppStore.getState().sessions)?.id).toBe(parent.id)
    await act(async () => root.render(<NativeConversation parent={parent} chatId={child.id}
        navigationKey={`pending-scroll-pane-${serial}`} autoFocus={false} onBack={() => {}} />))
    await flushFrames()
    expect(held).toHaveLength(1); expect(held[0].done).toBe(true)
    expect(held[0].envelope.payload).toEqual({ chatId: child.id, limit: 50, deferToolCallContent: true, messagesDigest: '' })
    region().scrollTop = 230
    await act(async () => region().dispatchEvent(new Event('scroll', { bubbles: true })))
    await flushFrames()
    const originalVisit = snapshotVisit()
    expect(region().scrollTop).toBeLessThan(region().scrollHeight - region().clientHeight - 100)
    const parentRows = useAppStore.getState().messages[parent.id]
    holdChild = true; revision = 'after'; total = 81
    const patch = { id: child.id, title: child.name, folderId: child.folderId, providerId: child.providerId,
        createdAt: child.createdAt.toISOString(), updatedAt: '2026-10-22T10:02:00Z',
        executionHostId: child.executionHostId, workspaceDirectory: child.workspaceDirectory,
        workspaceWorktreeDirectory: child.workspaceWorktreeDirectory, importedReadOnly: true,
        subagentParentChatId: parent.id, subagentParentCreatedAt: parent.createdAt.toISOString(),
        messageCount: total, messagesDigest: 'child-summary-after' }
    await act(async () => window.uamPush!({ type: 'statePatch', data: { stateRevision: 2, chats: [patch] } }))
    await flushFrames()
    expect(held).toHaveLength(2); expect(held[1].done).toBe(false)
    expect(held[1].envelope.payload).toEqual({ chatId: child.id, limit: 50, deferToolCallContent: true, messagesDigest: 'child-page-before' })
    expect(useAppStore.getState().sessions.find(candidate => candidate.id === child.id)?.messageCount).toBe(81)
    expect(rows().at(-1)?.dataset.messageIndex).toBe('79')
    return { originalVisit, parentRows }
}
function assertPendingScrollIsReadOnly(parentRows: ReturnType<typeof useAppStore.getState>['messages'][string]) {
    expect(held).toHaveLength(2)
    expect(host.textContent).not.toContain('This child catalogue is unavailable')
    expect(host.querySelector('textarea, input, form')).toBeNull()
    const state = useAppStore.getState()
    expect(state.activeSessionId).toBe(parent.id); expect(state.chatSearchNavigation).toBeNull()
    expect(state.messages[parent.id]).toBe(parentRows)
    expect(state.sendAcpPrompt).not.toHaveBeenCalled(); expect(state.startAcpSession).not.toHaveBeenCalled()
    expect(state.stopAcpSession).not.toHaveBeenCalled(); expect(state.retryFailedMessage).not.toHaveBeenCalled()
    expect(state.resumeQueuedAcpPrompts).not.toHaveBeenCalled()
    expect(envelopes.filter(item => item.action !== 'getChatMessages' && item.action !== 'releaseChatMessages')).toEqual([])
    expect(envelopes.filter(item => item.payload?.chatId === parent.id || item.payload?.refreshNative)).toEqual([])
}

describe('reader intent changes during actual summary-only append read, authored UNRUN', () => {
    it('retains the most recent visible row and relative offset when the reader scrolls off-tail during the held read', async () => {
        const { originalVisit, parentRows } = await beginPendingAppendFromOffTail()
        const range = region().scrollHeight - region().clientHeight
        expect(range).toBeGreaterThan(700)
        // Stay away from both page edges and establish a distinct actual row and pixel offset.
        region().scrollTop = Math.floor(range / 2) + 17
        await act(async () => region().dispatchEvent(new Event('scroll', { bubbles: true })))
        await flushFrames()
        const latest = snapshotVisit()
        expect(latest.index).not.toBe(originalVisit.index)
        expect(latest.offset).not.toBe(originalVisit.offset)
        expect(region().scrollTop).toBeGreaterThan(320)
        expect(range - region().scrollTop).toBeGreaterThan(320)
        expect(held[1].done).toBe(false)
        assertPendingScrollIsReadOnly(parentRows)
        await act(async () => held[1].finish())
        await flushFrames()
        const acceptedVisit = snapshotVisit()
        expect({ index: acceptedVisit.index, text: acceptedVisit.text, offset: acceptedVisit.offset })
            .toEqual({ index: latest.index, text: latest.text, offset: latest.offset })
        expect(host.textContent).toContain('Native summary history row 80')
        assertPendingScrollIsReadOnly(parentRows)
    })

    it('follows the new actual tail after the reader reaches the retained old tail during the held read', async () => {
        const { parentRows } = await beginPendingAppendFromOffTail()
        expect(rows().at(-1)?.dataset.messageIndex).toBe('79')
        region().scrollTop = region().scrollHeight - region().clientHeight
        await act(async () => region().dispatchEvent(new Event('scroll', { bubbles: true })))
        await flushFrames()
        expect(region().scrollTop).toBe(region().scrollHeight - region().clientHeight)
        expect(held[1].done).toBe(false)
        assertPendingScrollIsReadOnly(parentRows)
        await act(async () => held[1].finish())
        await flushFrames()
        expect(rows().at(-1)?.dataset.messageIndex).toBe('80')
        expect(rows().at(-1)?.textContent).toContain('Native summary history row 80')
        expect(region().scrollTop).toBe(region().scrollHeight - region().clientHeight)
        assertPendingScrollIsReadOnly(parentRows)
    })
})
