import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { NativeConversation } from './SubagentPaneContent'
import { useAppStore } from '../../store/useAppStore'
import type { Session } from '../../types/session'
import { fallbackProviderForId } from '../../utils/providerMetadata'
import { subagentParentFor } from '../../utils/subagentNavigation'
import { buildMessageFromCpp } from '../../store/cpp/reconcile'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

type Dimension = 'child-birth' | 'parent-birth' | 'provider' | 'host' | 'workspace' | 'worktree' | 'folder' | 'lineage' | 'digest' | 'total' | 'pane' | 'passive-eligibility'
type Envelope = { action: string; requestId?: string; payload?: Record<string, unknown> }
type Settlement = 'success' | 'error'
type HeldRead = { envelope: Envelope; done: boolean; finish: (outcome: Settlement) => void }
const dimensions: Dimension[] = ['child-birth', 'parent-birth', 'provider', 'host', 'workspace', 'worktree', 'folder', 'lineage', 'digest', 'total', 'pane', 'passive-eligibility']
const staleCases = dimensions.flatMap(dimension => (['success', 'error'] as const).map(outcome => ({ dimension, outcome })))
const staleError = 'OBSOLETE TARGET REQUEST ERROR MUST NOT ENTER CURRENT VIEW'
const targetText = 'ORIGINAL OWNED TARGET PAYLOAD ROW ZERO'
let serial = 0
let previous: ReturnType<typeof useAppStore.getState>, previousCef: Window['cefQuery']
let host: HTMLDivElement, root: Root, parent: Session, child: Session, navigationKey: string
let envelopes: Envelope[], held: HeldRead[], settledEnvelopes: number
let alignmentDescriptor: PropertyDescriptor | undefined
let frames: Map<number, FrameRequestCallback>, frameId: number
const wire = (start: number, end: number) => Array.from({ length: end - start }, (_, offset) => {
    const index = start + offset
    return { role: index % 2 ? 'assistant' as const : 'user' as const,
        content: index === 0 ? targetText : `Canonical child history row ${index}`,
        createdAt: new Date(Date.parse('2026-10-22T10:00:00Z') + index * 1000).toISOString() }
})
const indices = (total: number) => Array.from({ length: Math.ceil(total / 2) }, (_, index) => index * 2)
const ownRow = (index: number) => host.querySelector<HTMLElement>(`[data-message-index="${index}"]`)
const render = async () => { await act(async () => root.render(<NativeConversation parent={parent} chatId={child.id} navigationKey={navigationKey} autoFocus={false} onBack={() => {}} />)) }

beforeEach(() => {
    previous = useAppStore.getState(); previousCef = window.cefQuery; serial += 1
    const createdAt = new Date('2026-10-22T09:00:00Z')
    parent = { id: `history-owner-parent-${serial}`, name: 'Owned history parent', providerId: 'codex-cli', folderId: 'owned-folder',
        viewMode: 'chat', createdAt, updatedAt: createdAt, executionHostId: 'local', workspaceDirectory: '/workspace/owned',
        workspaceWorktreeDirectory: '', messagesDigest: 'owned-parent-digest', messageCount: 2, userMessageIndices: [0] }
    child = { ...parent, id: `history-owner-child-${serial}`, name: 'Owned passive child', importedReadOnly: true,
        createdAt: new Date('2026-10-22T09:01:00Z'), subagentParentChatId: parent.id,
        subagentParentCreatedAt: parent.createdAt.toISOString(), messagesDigest: 'owned-child-digest-v1', messageCount: 1200,
        userMessageIndices: indices(1200) }
    navigationKey = `owned-pane-${serial}-A`
    envelopes = []; held = []; settledEnvelopes = 0; frames = new Map(); frameId = 0
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
        const payload = envelope.payload
        if (envelope.action === 'getChatMessages' && payload?.targetMessageIndex !== undefined) {
            // Capture the old catalogue exactly when the real consumer issues the target read.
            const captured = useAppStore.getState().sessions.find(session => session.id === payload.chatId)!
            const pending: HeldRead = { envelope, done: false, finish: outcome => {
                if (pending.done) return
                pending.done = true; settledEnvelopes += 1
                onSuccess(JSON.stringify(outcome === 'error' ? { ok: false, error: staleError } : { ok: true, data: {
                    chatId: captured.id, messagesDigest: captured.messagesDigest, startIndex: 0, totalCount: captured.messageCount,
                    userMessageIndices: captured.userMessageIndices, messages: wire(0, 26),
                } }))
            } }
            held.push(pending)
        } else {
            settledEnvelopes += 1
            if (envelope.action === 'getChatMessages') {
                const current = useAppStore.getState().sessions.find(session => session.id === payload?.chatId)!
                const total = current?.messageCount ?? 0
                const start = Math.max(0, total - 50)
                onSuccess(JSON.stringify(current && payload?.limit === 50 && payload?.before === undefined && !payload?.refreshNative
                    ? { ok: true, data: { chatId: current.id, messagesDigest: current.messagesDigest, startIndex: start,
                        totalCount: total, userMessageIndices: indices(total), messages: wire(start, total) } }
                    : { ok: false, error: 'Fixture refuses an unbounded, refreshed or unexpected history read.' }))
            } else onSuccess(JSON.stringify({ ok: envelope.action === 'releaseChatMessages', data: {} }))
        }
    }
    useAppStore.setState({ sessions: [parent, child], providers: [fallbackProviderForId('codex-cli')], folders: [], resourceCollections: [],
        activeSessionId: parent.id, messages: { [parent.id]: wire(0, 2).map((row, index) => buildMessageFromCpp(parent.id, row, index)),
            [child.id]: wire(1150, 1200).map((row, offset) => buildMessageFromCpp(child.id, row, 1150 + offset)) },
        historyStartIndexBySessionId: { [parent.id]: 0, [child.id]: 1150 }, chatHistoryErrorBySessionId: {}, chatSearchNavigation: null,
        acpBindingBySessionId: {}, cliBindingBySessionId: {}, markdownStoreAttachedBySessionId: {},
        sendAcpPrompt: vi.fn().mockResolvedValue(true), startAcpSession: vi.fn().mockResolvedValue(true),
        stopAcpSession: vi.fn().mockResolvedValue(true), retryFailedMessage: vi.fn().mockResolvedValue({ ok: false }),
        resumeQueuedAcpPrompts: vi.fn().mockResolvedValue(false) })
    host = document.createElement('div'); document.body.append(host); root = createRoot(host)
    const positions = new WeakMap<Element, number>()
    const isRegion = (element: Element) => element.classList.contains('uam-subagent-transcript')
    const rows = (element: Element) => [...element.querySelectorAll('[data-message-index]')]
    const height = (element: Element) => 40 + ((element.textContent?.length ?? 0) % 3) * 4
    const offset = (element: Element, region: Element) => { let top = 0; for (const row of rows(region)) { if (row === element) return top; top += height(row) } return top }
    const rect = (top: number, size: number) => ({ x: 0, y: top, top, bottom: top + size, left: 0, right: 400, width: 400, height: size, toJSON: () => ({}) }) as DOMRect
    vi.spyOn(Element.prototype, 'scrollHeight', 'get').mockImplementation(function (this: Element) { return isRegion(this) ? rows(this).reduce((sum, row) => sum + height(row), 0) : 0 })
    vi.spyOn(Element.prototype, 'clientHeight', 'get').mockImplementation(function (this: Element) { return isRegion(this) ? 300 : 0 })
    vi.spyOn(Element.prototype, 'scrollTop', 'get').mockImplementation(function (this: Element) { return positions.get(this) ?? 0 })
    vi.spyOn(Element.prototype, 'scrollTop', 'set').mockImplementation(function (this: Element, value: number) { positions.set(this, Math.max(0, Math.min(value, Math.max(0, this.scrollHeight - this.clientHeight)))) })
    vi.spyOn(Element.prototype, 'getBoundingClientRect').mockImplementation(function (this: Element) {
        if (isRegion(this)) return rect(100, 300)
        if (this.getAttribute('role') === 'slider') return rect(20, 200)
        const region = this.closest('.uam-subagent-transcript')
        return region && this.hasAttribute('data-message-index') ? rect(100 + offset(this, region) - region.scrollTop, height(this)) : rect(0, 0)
    })
    alignmentDescriptor = Object.getOwnPropertyDescriptor(Element.prototype, 'scrollIntoView')
    Object.defineProperty(Element.prototype, 'scrollIntoView', { configurable: true, value: function (this: Element) {
        const region = this.closest('.uam-subagent-transcript'); if (region && this.hasAttribute('data-message-index')) region.scrollTop = offset(this, region)
    } })
})

afterEach(async () => {
    try {
        // Unmount first; even failed prerequisites/oracles must resolve every held old target callback.
        await act(async () => { root.unmount(); for (const pending of held) pending.finish('error') })
        await act(async () => { previous.unloadSessionMessages(child.id) })
        expect(held.every(pending => pending.done)).toBe(true)
        expect(settledEnvelopes).toBe(envelopes.length)
    } finally {
        window.cefQuery = previousCef; useAppStore.setState(previous, true); host.remove(); frames.clear()
        if (alignmentDescriptor) Object.defineProperty(Element.prototype, 'scrollIntoView', alignmentDescriptor)
        else Reflect.deleteProperty(Element.prototype, 'scrollIntoView')
        vi.unstubAllGlobals(); vi.restoreAllMocks()
    }
})

async function establishHeldOwnedTarget() {
    const state = useAppStore.getState()
    expect(state.loadSessionMessages).toBe(previous.loadSessionMessages)
    expect(state.setActiveSession).toBe(previous.setActiveSession)
    expect(vi.isMockFunction(state.loadSessionMessages)).toBe(false)
    expect(vi.isMockFunction(state.setActiveSession)).toBe(false)
    expect(Number.isFinite(child.createdAt.getTime())).toBe(true)
    expect(Number.isFinite(parent.createdAt.getTime())).toBe(true)
    expect(subagentParentFor(child, state.sessions)?.id).toBe(parent.id)
    expect(child.importedReadOnly).toBe(true)
    await render()
    const current = useAppStore.getState().sessions.find(session => session.id === child.id)!
    expect(current.messagesDigest).toBe('owned-child-digest-v1')
    expect(current.messageCount).toBe(1200); expect(current.userMessageIndices).toEqual(indices(1200))
    expect(envelopes.some(item => item.action === 'getChatMessages' && item.payload?.chatId === child.id && item.payload?.limit === 50 && item.payload?.before === undefined)).toBe(true)
    expect(ownRow(1198)?.textContent).toContain('Canonical child history row 1198')
    expect(ownRow(0)).toBeNull()
    const region = host.querySelector<HTMLElement>('[role="region"]')!
    expect(region.scrollHeight).toBeGreaterThan(region.clientHeight + 320)
    const rail = host.querySelector<HTMLElement>('[role="slider"][aria-label="Navigate user messages"]')!
    expect(rail).not.toBeNull(); expect(rail.getAttribute('aria-valuemax')).toBe('600')
    expect(held).toHaveLength(0)
    await act(async () => { rail.dispatchEvent(new MouseEvent('click', { bubbles: true, clientY: 20 })) })
    expect(held).toHaveLength(1)
    expect(held[0].done).toBe(false)
    expect(held[0].envelope).toMatchObject({ action: 'getChatMessages', requestId: expect.any(String), payload: {
        chatId: child.id, limit: 50, before: 26, deferToolCallContent: true, expectedMessagesDigest: 'owned-child-digest-v1', targetMessageIndex: 0,
    } })
    expect(useAppStore.getState().activeSessionId).toBe(parent.id)
    expect(useAppStore.getState().chatSearchNavigation).toBeNull()
    return held[0]
}

async function mutateOneCurrentIdentity(dimension: Dimension) {
    const state = useAppStore.getState()
    const beforeChild = state.sessions.find(session => session.id === child.id)!
    const beforeParent = state.sessions.find(session => session.id === parent.id)!
    child = beforeChild; parent = beforeParent
    switch (dimension) {
        case 'child-birth': child = { ...child, createdAt: new Date(child.createdAt.getTime() + 1) }; break
        case 'parent-birth': parent = { ...parent, createdAt: new Date(parent.createdAt.getTime() + 1) }; break
        case 'provider': child = { ...child, providerId: 'gemini-cli' }; break
        case 'host': child = { ...child, executionHostId: 'other-host' }; break
        case 'workspace': child = { ...child, workspaceDirectory: '/workspace/other' }; break
        case 'worktree': child = { ...child, workspaceWorktreeDirectory: '/workspace/other-worktree' }; break
        case 'folder': child = { ...child, folderId: 'other-folder' }; break
        case 'lineage': child = { ...child, subagentParentChatId: 'unavailable-parent' }; break
        case 'digest': child = { ...child, messagesDigest: 'owned-child-digest-v2' }; break
        case 'total': child = { ...child, messageCount: 1202 }; break
        case 'pane': navigationKey += '-B'; break
        case 'passive-eligibility': child = { ...child, importedReadOnly: false }; break
    }
    const changed = (before: Session, after: Session) => (Object.keys(after) as (keyof Session)[]).filter(key => !Object.is(before[key], after[key]))
    expect(changed(beforeChild, child).length + changed(beforeParent, parent).length + Number(dimension === 'pane')).toBe(1)
    await act(async () => {
        useAppStore.setState({ sessions: [parent, child] })
        root.render(<NativeConversation parent={parent} chatId={child.id} navigationKey={navigationKey} autoFocus={false} onBack={() => {}} />)
    })
    const live = useAppStore.getState()
    const currentChild = live.sessions.find(session => session.id === child.id)!
    const currentAvailable = subagentParentFor(currentChild, live.sessions)?.id === parent.id
    if (currentAvailable) {
        const lastUser = (currentChild.messageCount ?? 0) - 2
        expect(ownRow(lastUser)?.textContent).toContain(`Canonical child history row ${lastUser}`)
    } else expect(host.textContent).toContain('This native conversation is unavailable.')
    expect(held).toHaveLength(1) // New current view may hydrate, never implicitly repeats the old target.
    return currentAvailable
}

function expectNoInputOrGlobalNavigation() {
    const state = useAppStore.getState()
    expect(state.activeSessionId).toBe(parent.id); expect(state.chatSearchNavigation).toBeNull()
    expect(host.querySelector('textarea, input, form')).toBeNull()
    expect([...host.querySelectorAll('button')].some(button => /^(Follow up|Stop|Retry message|Resume)/i.test(button.getAttribute('aria-label') || button.textContent?.trim() || ''))).toBe(false)
    expect(state.sendAcpPrompt).not.toHaveBeenCalled(); expect(state.startAcpSession).not.toHaveBeenCalled()
    expect(state.stopAcpSession).not.toHaveBeenCalled(); expect(state.retryFailedMessage).not.toHaveBeenCalled()
    expect(state.resumeQueuedAcpPrompts).not.toHaveBeenCalled()
    expect(envelopes.filter(item => item.action === 'selectSession' || item.payload?.refreshNative === true)).toEqual([])
    expect(envelopes.filter(item => item.action !== 'getChatMessages' && item.action !== 'releaseChatMessages')).toEqual([])
}

describe('actual NativeConversation held history ownership, authored UNRUN', () => {
    it('positively adopts the same-owner bounded target through real initial hydration without shared state mutation', async () => {
        const pending = await establishHeldOwnedTarget()
        const beforeReply = useAppStore.getState()
        await act(async () => pending.finish('success'))
        expect(ownRow(0)?.textContent).toContain(targetText)
        expect(useAppStore.getState()).toBe(beforeReply)
        expect(useAppStore.getState().messages[child.id]).toHaveLength(50)
        expect(useAppStore.getState().historyStartIndexBySessionId[child.id]).toBe(1150)
        expectNoInputOrGlobalNavigation()
    })
    it.each(staleCases)('rejects held old $outcome after actual $dimension changes', async ({ dimension, outcome }) => {
        const pending = await establishHeldOwnedTarget()
        const available = await mutateOneCurrentIdentity(dimension)
        const beforeReply = useAppStore.getState()
        const beforeHtml = host.innerHTML
        await act(async () => pending.finish(outcome))
        expect(pending.done).toBe(true)
        expect(ownRow(0)).toBeNull(); expect(host.textContent).not.toContain(targetText)
        expect(host.textContent).not.toContain(staleError)
        expect(host.innerHTML).toBe(beforeHtml)
        expect(useAppStore.getState()).toBe(beforeReply)
        expect(useAppStore.getState().chatHistoryErrorBySessionId[child.id]).toBeUndefined()
        if (!available) expect(host.textContent).toContain('This native conversation is unavailable.')
        expectNoInputOrGlobalNavigation()
    })
})
