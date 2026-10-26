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
                    chatId: captured.id, messagesDigest: captured.messagesDigest, startIndex: Math.max(0, Math.min(captured.messageCount!, Number(payload?.before)) - 50), totalCount: captured.messageCount,
                    userMessageIndices: captured.userMessageIndices, messages: wire(Math.max(0, Math.min(captured.messageCount!, Number(payload?.before)) - 50), Math.min(captured.messageCount!, Number(payload?.before))),
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

function expectNoInputOrGlobalNavigation(allowNativeRefresh = false) {
    const state = useAppStore.getState()
    expect(state.activeSessionId).toBe(parent.id); expect(state.chatSearchNavigation).toBeNull()
    expect(host.querySelector('textarea, input, form')).toBeNull()
    expect([...host.querySelectorAll('button')].some(button => /^(Follow up|Stop|Retry message|Resume)/i.test(button.getAttribute('aria-label') || button.textContent?.trim() || ''))).toBe(false)
    expect(state.sendAcpPrompt).not.toHaveBeenCalled(); expect(state.startAcpSession).not.toHaveBeenCalled()
    expect(state.stopAcpSession).not.toHaveBeenCalled(); expect(state.retryFailedMessage).not.toHaveBeenCalled()
    expect(state.resumeQueuedAcpPrompts).not.toHaveBeenCalled()
    expect(envelopes.filter(item => item.action === 'selectSession' || item.payload?.refreshNative === true)).toEqual([])
    expect(envelopes.filter(item => item.action !== 'getChatMessages' && item.action !== 'releaseChatMessages' &&
        !(allowNativeRefresh && item.action === 'openNativeSessionChat'))).toEqual([])
}

describe('passive native-journal refresh refusal feedback', () => {
    const refusal = "Native child history is shorter than the saved conversation. Saved history was retained for reading. Refresh can succeed when this child's complete native history is available; the same shorter source cannot replace it."
    const generic = 'The native conversation could not be read. Refresh to try again.'

    function prepareJournal(loaded: 'missing' | 'empty' | 'tail' = 'tail') {
        const tail = useAppStore.getState().messages[child.id]
        parent = { ...parent, providerId: 'gemini-cli' }
        child = { ...child, id: `gemini-agent-refusal-${serial}`, providerId: 'gemini-cli' }
        useAppStore.setState(state => ({ sessions: [parent, child], providers: [fallbackProviderForId('gemini-cli')],
            messages: { [parent.id]: state.messages[parent.id], ...(loaded === 'missing' ? {} : {
                [child.id]: loaded === 'empty' ? [] : tail.map(row => ({ ...row, sessionId: child.id })),
            }) }, historyStartIndexBySessionId: { [parent.id]: 0, [child.id]: 1150 } }))
        expect(useAppStore.getState().openSubAgentSession).toBe(previous.openSubAgentSession)
        expect(useAppStore.getState().loadSessionMessages).toBe(previous.loadSessionMessages)
    }

    it('shows a new refusal after dismissing an earlier saved-read error with messages retained', async () => {
        prepareJournal()
        const realCef = window.cefQuery!
        let firstRead = true
        window.cefQuery = args => {
            const envelope = JSON.parse(args.request) as Envelope
            if (envelope.action === 'openNativeSessionChat') {
                envelopes.push(envelope); settledEnvelopes += 1; args.onFailure(502, refusal); return
            }
            if (envelope.action === 'getChatMessages' && firstRead) {
                firstRead = false; envelopes.push(envelope); settledEnvelopes += 1
                args.onFailure(503, 'Temporary saved history read failure.'); return
            }
            realCef(args)
        }
        await render()
        const savedMessages = useAppStore.getState().messages[child.id]
        expect(ownRow(1198)?.textContent).toContain('Canonical child history row 1198')
        expect(host.querySelector('[role="alert"] p')?.textContent).toContain(generic)
        await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Dismiss native conversation error"]')!.click())
        expect(host.querySelector('[role="alert"]')).toBeNull()
        await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Refresh conversation"]')!.click())
        expect(host.querySelector('[role="alert"] p')?.textContent).toBe(refusal)
        expect(host.querySelector('[role="alert"] details')).toBeNull()
        expect(useAppStore.getState().messages[child.id]).toBe(savedMessages)
        expect(ownRow(1198)?.textContent).toContain('Canonical child history row 1198')
        expect(envelopes.filter(item => item.action === 'getChatMessages')).toHaveLength(1)
        expect(envelopes.filter(item => item.action === 'openNativeSessionChat')).toHaveLength(1)
        expectNoInputOrGlobalNavigation(true)
    })

    it.each(['missing', 'empty'] as const)('recovers saved history after an initial failed read and refusal with %s messages', async loaded => {
        prepareJournal(loaded)
        const parentMessages = useAppStore.getState().messages[parent.id]
        const realCef = window.cefQuery!
        let reads = 0
        window.cefQuery = args => {
            const envelope = JSON.parse(args.request) as Envelope
            if (envelope.action === 'openNativeSessionChat') {
                envelopes.push(envelope); settledEnvelopes += 1
                args.onFailure(502, refusal); return
            }
            if (envelope.action === 'getChatMessages' && ++reads === 1) {
                envelopes.push(envelope); settledEnvelopes += 1
                args.onFailure(503, 'Temporary saved history read failure.'); return
            }
            realCef(args)
        }
        await render()
        expect(ownRow(1198)).toBeNull()
        expect(useAppStore.getState().chatHistoryErrorBySessionId[child.id]).toBeDefined()
        await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Refresh conversation"]')!.click())
        expect(ownRow(1198)?.textContent).toContain('Canonical child history row 1198')
        expect(host.querySelector('[role="alert"] p')?.textContent).toBe(refusal)
        expect(host.querySelector('[role="alert"] details')).toBeNull()
        expect(host.textContent).not.toContain('This native conversation is unavailable.')
        expect(host.textContent).not.toContain('Loaded messages are still shown.')
        expect(useAppStore.getState().chatHistoryErrorBySessionId[child.id]).toBeUndefined()
        expect(useAppStore.getState().messages[parent.id]).toBe(parentMessages)
        expect(useAppStore.getState().chatHistoryPageBySessionId[child.id]).toMatchObject({
            chatId: child.id, birth: child.createdAt.getTime(), summaryDigest: child.messagesDigest, totalCount: 1200,
        })
        const savedReads = envelopes.filter(item => item.action === 'getChatMessages')
        expect(savedReads).toHaveLength(2)
        savedReads.forEach(item => expect(item.payload).toEqual({ chatId: child.id, limit: 50, deferToolCallContent: true, messagesDigest: '' }))
        expect(envelopes.filter(item => item.action === 'openNativeSessionChat')).toHaveLength(1)
        await drainProgrammaticScrollFrames()
        expectNoInputOrGlobalNavigation(true)
    })

    it.each(['missing', 'empty'] as const)('explains a failed saved recovery after refusal with %s messages', async loaded => {
        prepareJournal(loaded)
        const parentMessages = useAppStore.getState().messages[parent.id]
        const realCef = window.cefQuery!
        window.cefQuery = args => {
            const envelope = JSON.parse(args.request) as Envelope
            if (envelope.action === 'openNativeSessionChat' || envelope.action === 'getChatMessages') {
                envelopes.push(envelope); settledEnvelopes += 1
                args.onFailure(503, envelope.action === 'openNativeSessionChat' ? refusal : 'SAVED READ DIAGNOSTIC FROM A DIFFERENT OPERATION')
                return
            }
            realCef(args)
        }
        await render()
        await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Refresh conversation"]')!.click())
        expect(host.querySelector('[role="alert"] p')?.textContent).toBe(refusal)
        expect(host.querySelector('[role="status"]')?.textContent).toBe('Saved history was retained, but could not be read this time. Refresh to try reading it again.')
        expect(host.querySelector('[role="alert"] details')).toBeNull()
        expect(host.textContent).not.toContain('The provider has not supplied a readable transcript.')
        expect(ownRow(1198)).toBeNull()
        expect(useAppStore.getState().messages[parent.id]).toBe(parentMessages)
        const reads = envelopes.filter(item => item.action === 'getChatMessages')
        expect(reads).toHaveLength(2)
        reads.forEach(item => expect(item.payload).toEqual({ chatId: child.id, limit: 50, deferToolCallContent: true, messagesDigest: '' }))
        expect(envelopes.filter(item => item.action === 'openNativeSessionChat')).toHaveLength(1)
        expectNoInputOrGlobalNavigation(true)
    })

    it.each(['success', 'error'] as const)('keeps the refusal across a count-only saved-history %s', async outcome => {
        prepareJournal()
        const realCef = window.cefQuery!
        let countChanged = false
        window.cefQuery = args => {
            const envelope = JSON.parse(args.request) as Envelope
            if (envelope.action === 'openNativeSessionChat' || countChanged && outcome === 'error' && envelope.action === 'getChatMessages') {
                envelopes.push(envelope); settledEnvelopes += 1
                args.onFailure(503, envelope.action === 'openNativeSessionChat' ? refusal : 'BACKGROUND SAVED READ DIAGNOSTIC')
                return
            }
            realCef(args)
        }
        await render()
        const parentMessages = useAppStore.getState().messages[parent.id]
        await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Refresh conversation"]')!.click())
        expect(host.querySelector('[role="alert"] p')?.textContent).toBe(refusal)
        const retainedMessages = useAppStore.getState().messages[child.id]
        child = { ...child, messageCount: 1202, userMessageIndices: indices(1202) }
        countChanged = true
        await act(async () => useAppStore.setState({ sessions: [parent, child] }))
        await render()
        expect(host.querySelector('[role="alert"] p')?.textContent).toBe(refusal)
        expect(host.querySelector('[role="alert"] details')).toBeNull()
        if (outcome === 'error') expect(useAppStore.getState().messages[child.id]).toBe(retainedMessages)
        else expect(ownRow(1200)?.textContent).toContain('Canonical child history row 1200')
        expect(useAppStore.getState().messages[parent.id]).toBe(parentMessages)
        expect(envelopes.filter(item => item.action === 'getChatMessages')).toHaveLength(2)
        expect(envelopes.filter(item => item.action === 'openNativeSessionChat')).toHaveLength(1)
        expectNoInputOrGlobalNavigation(true)
    })

    it('refuses a different existing chat before recording lineage or reading its saved history', async () => {
        prepareJournal()
        const other: Session = { ...child, id: `unrelated-child-${serial}`, subagentParentChatId: undefined,
            subagentParentCreatedAt: undefined, messagesDigest: 'unrelated-digest', messageCount: 50, userMessageIndices: indices(50) }
        const unrelatedMessages = wire(0, 50).map((row, index) => buildMessageFromCpp(other.id, row, index))
        useAppStore.setState(state => ({ sessions: [parent, child, other], messages: { ...state.messages, [other.id]: unrelatedMessages } }))
        expect(subagentParentFor(other, useAppStore.getState().sessions)).toBeUndefined()
        const realCef = window.cefQuery!
        window.cefQuery = args => {
            const envelope = JSON.parse(args.request) as Envelope
            if (envelope.action !== 'openNativeSessionChat') { realCef(args); return }
            envelopes.push(envelope); settledEnvelopes += 1
            args.onSuccess(JSON.stringify({ ok: true, data: { chatId: other.id } }))
        }
        await render()
        const savedMessages = useAppStore.getState().messages[child.id]
        await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Refresh conversation"]')!.click())
        expect(host.querySelector('[role="alert"] p')?.textContent).toBe(generic + ' Loaded messages are still shown.')
        expect(subagentParentFor(other, useAppStore.getState().sessions)).toBeUndefined()
        expect(envelopes.filter(item => item.action === 'getChatMessages' && item.payload?.chatId === other.id)).toEqual([])
        expect(useAppStore.getState().messages[other.id]).toBe(unrelatedMessages)
        expect(useAppStore.getState().messages[child.id]).toBe(savedMessages)
        expectNoInputOrGlobalNavigation(true)
    })

    it.each(['pane', 'child-birth', 'digest'] as const)('rejects an old saved recovery after a newer refusal and %s change', async dimension => {
        prepareJournal('missing')
        const realCef = window.cefQuery!
        let reads = 0, finishOld: (() => void) | undefined
        window.cefQuery = args => {
            const envelope = JSON.parse(args.request) as Envelope
            if (envelope.action === 'openNativeSessionChat') {
                envelopes.push(envelope); settledEnvelopes += 1; args.onFailure(502, refusal); return
            }
            if (envelope.action === 'getChatMessages') {
                reads += 1
                if (reads === 1 || reads === 3) {
                    envelopes.push(envelope); settledEnvelopes += 1
                    args.onFailure(503, 'Temporary saved read failure.'); return
                }
                if (reads === 2) {
                    const captured = child
                    envelopes.push(envelope)
                    finishOld = () => {
                        finishOld = undefined; settledEnvelopes += 1
                        args.onSuccess(JSON.stringify({ ok: true, data: { chatId: captured.id, messagesDigest: captured.messagesDigest,
                            startIndex: 1150, totalCount: 1200, userMessageIndices: indices(1200),
                            messages: wire(1150, 1200).map(row => ({ ...row, content: 'OBSOLETE SAVED RECOVERY MUST NOT REPLACE CURRENT MESSAGES' })) } }))
                    }
                    return
                }
            }
            realCef(args)
        }
        try {
            await render()
            await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Refresh conversation"]')!.click())
            expect(finishOld).toBeDefined()
            if (dimension === 'pane') navigationKey += '-new'
            else if (dimension === 'child-birth') child = { ...child, createdAt: new Date(child.createdAt.getTime() + 1) }
            else child = { ...child, messagesDigest: 'new-current-digest' }
            await act(async () => useAppStore.setState({ sessions: [parent, child] }))
            await render()
            await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Refresh conversation"]')!.click())
            expect(ownRow(1198)?.textContent).toContain('Canonical child history row 1198')
            const currentMessages = useAppStore.getState().messages[child.id]
            const currentPage = useAppStore.getState().chatHistoryPageBySessionId[child.id]
            await act(async () => finishOld!())
            expect(useAppStore.getState().messages[child.id]).toBe(currentMessages)
            expect(useAppStore.getState().chatHistoryPageBySessionId[child.id]).toBe(currentPage)
            expect(host.querySelector('[role="alert"] p')?.textContent).toBe(refusal)
            expect(host.textContent).not.toContain('OBSOLETE SAVED RECOVERY')
            expectNoInputOrGlobalNavigation(true)
        } finally { await act(async () => finishOld?.()) }
    })

    it.each(['transport', 'lookalike refusal', 'wrong chat id'] as const)('uses safe primary copy for a %s response', async response => {
        prepareJournal()
        const realCef = window.cefQuery!
        const raw = response === 'lookalike refusal' ? refusal + ' RAW TRANSPORT DIAGNOSTICS' : '\u001b[31mSSH runner /private/diagnostic\u0000\u202e ' + 'x'.repeat(800)
        window.cefQuery = args => {
            const envelope = JSON.parse(args.request) as Envelope
            if (envelope.action !== 'openNativeSessionChat') { realCef(args); return }
            envelopes.push(envelope); settledEnvelopes += 1
            if (response === 'wrong chat id') args.onSuccess(JSON.stringify({ ok: true, data: { chatId: 'different-child' } }))
            else args.onFailure(502, raw)
        }
        await render()
        const savedMessages = useAppStore.getState().messages[child.id]
        await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Refresh conversation"]')!.click())
        expect(host.querySelector('[role="alert"] p')?.textContent).toBe(generic + ' Loaded messages are still shown.')
        expect(useAppStore.getState().messages[child.id]).toBe(savedMessages)
        expect(ownRow(1198)?.textContent).toContain('Canonical child history row 1198')
        const details = host.querySelector<HTMLDetailsElement>('[role="alert"] details')
        if (response === 'wrong chat id') expect(details).toBeNull()
        else {
            expect(details?.open).toBe(false)
            expect(details?.querySelector('summary')?.textContent).toBe('Technical details')
            const detail = details?.querySelector('pre')?.textContent || ''
            expect(detail.length).toBeGreaterThan(0); expect(detail.length).toBeLessThanOrEqual(401)
            expect(detail).not.toMatch(/[\u0000-\u001f\u007f-\u009f\u202a-\u202e\u2066-\u2069]/)
            await act(async () => { details!.open = true })
            expect(details?.open).toBe(true)
        }
        expect(envelopes.filter(item => item.action === 'openNativeSessionChat')).toHaveLength(1)
        expectNoInputOrGlobalNavigation(true)
    })

    it.each(['current pane', 'replaced pane'] as const)('retains saved messages and current refusal feedback in the %s', async mode => {
        const oldId = child.id
        parent = { ...parent, providerId: 'gemini-cli' }
        child = { ...child, id: `gemini-agent-refusal-${serial}`, providerId: 'gemini-cli' }
        useAppStore.setState(state => ({ sessions: [parent, child], providers: [fallbackProviderForId('gemini-cli')],
            messages: { ...state.messages, [child.id]: state.messages[oldId].map(row => ({ ...row, sessionId: child.id })) },
            historyStartIndexBySessionId: { ...state.historyStartIndexBySessionId, [child.id]: 1150 } }))
        expect(useAppStore.getState().openSubAgentSession).toBe(previous.openSubAgentSession)
        expect(vi.isMockFunction(useAppStore.getState().openSubAgentSession)).toBe(false)
        const imports: { done: boolean; finish: (success: boolean) => void }[] = []
        const realCef = window.cefQuery!
        window.cefQuery = args => {
            const envelope = JSON.parse(args.request) as Envelope
            if (envelope.action !== 'openNativeSessionChat') { realCef(args); return }
            envelopes.push(envelope)
            expect(envelope.payload).toMatchObject({ chatId: parent.id, nativeSessionId: child.id, selectChat: false, refreshHistory: true })
            const pending = { done: false, finish: (success: boolean) => {
                if (pending.done) return
                pending.done = true; settledEnvelopes += 1
                if (success) args.onSuccess(JSON.stringify({ ok: true, data: { chatId: child.id } }))
                else args.onFailure(502, refusal)
            } }
            imports.push(pending)
        }
        try {
            await render()
            const parentMessages = useAppStore.getState().messages[parent.id]
            expect(ownRow(1198)?.textContent).toContain('Canonical child history row 1198')
            const refresh = host.querySelector<HTMLButtonElement>('button[aria-label="Refresh conversation"]')!
            await act(async () => refresh.click())
            expect(imports).toHaveLength(1)
            expect(refresh.disabled).toBe(true)
            if (mode === 'replaced pane') {
                navigationKey += '-replacement'; await render()
                const currentRefresh = host.querySelector<HTMLButtonElement>('button[aria-label="Refresh conversation"]')!
                expect(currentRefresh.disabled).toBe(false)
                await act(async () => currentRefresh.click())
                expect(imports).toHaveLength(2)
            }
            const savedMessages = useAppStore.getState().messages[child.id]
            const readsBeforeRefusal = envelopes.filter(item => item.action === 'getChatMessages').length
            await act(async () => imports[imports.length - 1].finish(false))
            expect(envelopes.filter(item => item.action === 'openNativeSessionChat')).toHaveLength(imports.length)
            expect(envelopes.filter(item => item.action === 'getChatMessages')).toHaveLength(readsBeforeRefusal)
            expect(useAppStore.getState().messages[child.id]).toBe(savedMessages)
            expect(useAppStore.getState().messages[parent.id]).toBe(parentMessages)
            expect(ownRow(1198)?.textContent).toContain('Canonical child history row 1198')
            expect(host.querySelector('[role="alert"]')?.textContent).toContain(refusal)
            expect(host.textContent).not.toContain('Loaded messages are still shown.')
            expect(host.textContent).not.toContain('The native conversation could not be read. Refresh to try again.')
            if (mode === 'replaced pane') {
                // The old pane's delayed success must not clear the current refusal.
                await act(async () => imports[0].finish(true))
                expect(host.querySelector('[role="alert"]')?.textContent).toContain(refusal)
                expect(ownRow(1198)?.textContent).toContain('Canonical child history row 1198')
            }
            expect(host.querySelector<HTMLButtonElement>('button[aria-label="Refresh conversation"]')!.disabled).toBe(false)
            await drainProgrammaticScrollFrames()
            expect(envelopes.filter(item => item.action === 'openNativeSessionChat')).toHaveLength(imports.length)
            expectNoInputOrGlobalNavigation(true)
        } finally {
            await act(async () => imports.forEach(pending => pending.finish(false)))
        }
    })
})

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


// Separate additive frontier controls. The original 25 cases above remain byte-for-byte intact.
// AUTHORED UNRUN: these controls do not assert nonexistent shared-store adoption guards.
async function commitFirstScopedWindow() {
    const first = await establishHeldOwnedTarget()
    await act(async () => first.finish('success'))
    expect(ownRow(0)?.textContent).toContain(targetText)
    return first
}

async function startFarPendingTarget() {
    const rail = host.querySelector<HTMLElement>('[role="slider"][aria-label="Navigate user messages"]')!
    const before = held.length
    await act(async () => rail.dispatchEvent(new MouseEvent('click', { bubbles: true, clientY: 220 })))
    expect(held).toHaveLength(before + 1)
    const pending = held.at(-1)!
    expect(pending.envelope.payload?.targetMessageIndex).toBe(1198)
    expect(pending.done).toBe(false)
    expect(ownRow(0)?.textContent).toContain(targetText) // Pending intent is not a committed reading position.
    return pending
}

async function changeOnlyCatalogue(dimension: 'digest' | 'total') {
    const state = useAppStore.getState()
    const live = state.sessions.find(session => session.id === child.id)!
    child = dimension === 'digest' ? { ...live, messagesDigest: 'owned-child-digest-v2' } : { ...live, messageCount: 1202 }
    await act(async () => useAppStore.setState({ sessions: [parent, child] }))
}

describe('passive summary ownership continuation controls, authored UNRUN', () => {
    it('revalidates a distant committed anchor with one bounded current-owner target instead of replaying pending rail intent', async () => {
        await commitFirstScopedWindow()
        const obsolete = await startFarPendingTarget()
        const parentMessages = useAppStore.getState().messages[parent.id]
        await changeOnlyCatalogue('digest')
        expect(held).toHaveLength(3)
        const fresh = held[2]
        expect(fresh.envelope).toMatchObject({ action: 'getChatMessages', payload: {
            chatId: child.id, targetMessageIndex: 0, limit: 50, before: 26,
            expectedMessagesDigest: 'owned-child-digest-v2', deferToolCallContent: true,
        } })
        expect(held.filter(item => item.envelope.payload?.targetMessageIndex === 1198)).toHaveLength(1)
        expect(ownRow(0)?.textContent).toContain(targetText) // Available evidence remains readable pending validation.
        await act(async () => fresh.finish('success'))
        expect(ownRow(0)?.textContent).toContain(targetText)
        expect(useAppStore.getState().messages[parent.id]).toBe(parentMessages)
        expect(obsolete.done).toBe(false)
        expectNoInputOrGlobalNavigation()
    })

    it('uses force and reimport only for the current explicit Refresh, then conditionally reads a later summary', async () => {
        const oldId = child.id
        parent = { ...parent, providerId: 'claude-cli' }
        child = { ...child, id: `claude-agent-explicit-refresh-${serial}`, providerId: 'claude-cli' }
        useAppStore.setState(state => ({ sessions: [parent, child],
            messages: { ...state.messages, [child.id]: state.messages[oldId].map(row => ({ ...row, sessionId: child.id })) },
            historyStartIndexBySessionId: { ...state.historyStartIndexBySessionId, [child.id]: 1150 } }))
        const reimport = vi.spyOn(useAppStore.getState(), 'openSubAgentSession').mockResolvedValue(child.id)
        await render()
        const reads = () => envelopes.filter(item => item.action === 'getChatMessages' && item.payload?.chatId === child.id && item.payload?.targetMessageIndex === undefined)
        expect(reads()).toHaveLength(1)
        const refresh = [...host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.getAttribute('aria-label') === 'Refresh conversation')!
        expect(refresh.disabled).toBe(false)
        await act(async () => refresh.click())
        expect(reimport).toHaveBeenCalledTimes(1)
        expect(reimport).toHaveBeenCalledWith(parent.id, child.id, child.name, false, expect.any(Function))
        expect(reads()).toHaveLength(2)
        expect(reads()[1].payload?.messagesDigest).toBe('')
        await changeOnlyCatalogue('digest')
        expect(reads()).toHaveLength(3)
        expect(reads()[2].payload?.messagesDigest).toBe('owned-child-digest-v1')
        expect(reads()[2].payload?.refreshNative).toBeUndefined()
        expect(reimport).toHaveBeenCalledTimes(1)
        expectNoInputOrGlobalNavigation()
    })

    it.each((['digest', 'total'] as const).flatMap(dimension => (['success', 'error'] as const).map(outcome => ({ dimension, outcome }))))(
        'retires held old $outcome and its retry/alignment when same-display $dimension changes', async ({ dimension, outcome }) => {
            await commitFirstScopedWindow()
            const obsolete = await startFarPendingTarget()
            await changeOnlyCatalogue(dimension)
            expect(held).toHaveLength(3)
            const fresh = held[2]
            expect(fresh.envelope.payload?.targetMessageIndex).toBe(0)
            expect(fresh.done).toBe(false)
            const beforeReply = useAppStore.getState(), beforeHtml = host.innerHTML
            await act(async () => obsolete.finish(outcome))
            expect(host.innerHTML).toBe(beforeHtml)
            expect(useAppStore.getState()).toBe(beforeReply)
            expect(host.textContent).not.toContain(staleError)
            expect([...host.querySelectorAll('button')].filter(button => button.textContent?.trim() === 'Retry history')).toHaveLength(0)
            expect(fresh.done).toBe(false) // Old settlement cannot settle the new owner's busy work.
            expect(held.filter(item => item.envelope.payload?.targetMessageIndex === 1198)).toHaveLength(1)
            await act(async () => fresh.finish('success'))
            expect(ownRow(0)?.textContent).toContain(targetText)
            expectNoInputOrGlobalNavigation()
        })

    it('reopens a changed summary using the same display visit and a fresh bounded child read', async () => {
        await commitFirstScopedWindow()
        const parentMessages = useAppStore.getState().messages[parent.id]
        await act(async () => root.render(<div />))
        const before = held.length
        await changeOnlyCatalogue('digest')
        expect(held).toHaveLength(before) // An unmounted child does not start a read.
        await render()
        expect(held).toHaveLength(before + 1)
        const restored = held.at(-1)!
        expect(restored.envelope.payload).toMatchObject({ chatId: child.id, targetMessageIndex: 0,
            expectedMessagesDigest: 'owned-child-digest-v2', limit: 50, before: 26 })
        await act(async () => restored.finish('success'))
        expect(ownRow(0)?.textContent).toContain(targetText)
        expect(useAppStore.getState().messages[parent.id]).toBe(parentMessages)
        expectNoInputOrGlobalNavigation()
    })
})

// Root additive recovery controls; prior corrected 32 cases remain an exact prefix.
describe('accepted passive summary recovery without stale page authority', () => {
    it('retains a refused committed-range refresh and retries that current range without adjacent splicing', async () => {
        await commitFirstScopedWindow()
        const refresh = [...host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.getAttribute('aria-label') === 'Refresh conversation')!
        await act(async () => refresh.click())
        expect(held).toHaveLength(2)
        expect(held[1].envelope.payload).toMatchObject({ targetMessageIndex: 0, expectedMessagesDigest: 'owned-child-digest-v1' })
        await act(async () => held[1].finish('error'))
        expect(ownRow(0)?.textContent).toContain(targetText)
        const region = host.querySelector<HTMLElement>('[role="region"]')!
        const count = envelopes.length
        await act(async () => { region.scrollTop = region.scrollHeight; region.dispatchEvent(new Event('scroll')) })
        expect(envelopes).toHaveLength(count)
        const retry = [...host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent === 'Retry history')!
        expect(retry).toBeDefined()
        await act(async () => retry.click())
        expect(held).toHaveLength(3)
        expect(held[2].envelope.payload).toMatchObject({ targetMessageIndex: 0, limit: 50, expectedMessagesDigest: 'owned-child-digest-v1' })
        await act(async () => held[2].finish('success'))
        expect(ownRow(0)?.textContent).toContain(targetText)
        expect(host.textContent).not.toContain('The child history page was refused or changed.')
        expectNoInputOrGlobalNavigation()
    })

    it('does not authorize retained-range reads from a refused ordinary hydration and recovers only from a later accepted read', async () => {
        await commitFirstScopedWindow()
        const realCef = window.cefQuery!
        window.cefQuery = args => {
            const envelope = JSON.parse(args.request) as Envelope
            if (envelope.action === 'getChatMessages' && envelope.payload?.targetMessageIndex === undefined) {
                envelopes.push(envelope); settledEnvelopes += 1
                args.onSuccess(JSON.stringify({ ok: false, error: 'Current child tail refused for this read' }))
            } else realCef(args)
        }
        await changeOnlyCatalogue('digest')
        expect(held).toHaveLength(1)
        expect(ownRow(0)?.textContent).toContain(targetText)
        expect(host.textContent).toContain('Current child tail refused for this read')
        window.cefQuery = realCef
        const refresh = [...host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.getAttribute('aria-label') === 'Refresh conversation')!
        expect(refresh.disabled).toBe(false)
        await act(async () => refresh.click())
        expect(held).toHaveLength(2)
        expect(held[1].envelope.payload).toMatchObject({ targetMessageIndex: 0, expectedMessagesDigest: 'owned-child-digest-v2' })
        await act(async () => held[1].finish('success'))
        expect(ownRow(0)?.textContent).toContain(targetText)
        expectNoInputOrGlobalNavigation()
    })
})

describe('passive hydration total and retained shrink boundaries', () => {
    it('adopts a valid changed-total hydration without a redundant second read or stuck loading', async () => {
        const realCef = window.cefQuery!
        window.cefQuery = args => {
            const envelope = JSON.parse(args.request) as Envelope
            if (envelope.action === 'getChatMessages' && envelope.payload?.targetMessageIndex === undefined) {
                envelopes.push(envelope); settledEnvelopes += 1
                args.onSuccess(JSON.stringify({ ok: true, data: { chatId: child.id, messagesDigest: 'accepted-page-new-total',
                    startIndex: 1152, totalCount: 1202, userMessageIndices: indices(1202), messages: wire(1152, 1202) } }))
            } else realCef(args)
        }
        await render()
        expect(envelopes.filter(item => item.action === 'getChatMessages')).toHaveLength(1)
        expect(useAppStore.getState().sessions.find(session => session.id === child.id)?.messageCount).toBe(1202)
        expect(ownRow(1200)?.textContent).toContain('Canonical child history row 1200')
        const refresh = [...host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.getAttribute('aria-label') === 'Refresh conversation')!
        expect(refresh.disabled).toBe(false)
        expectNoInputOrGlobalNavigation()
    })

    it('retains an unavailable anchor after shrink and offers truthful current-history recovery', async () => {
        await commitFirstScopedWindow()
        const region = host.querySelector<HTMLElement>('[role="region"]')!
        await act(async () => { region.scrollTop = region.scrollHeight - region.clientHeight; region.dispatchEvent(new Event('scroll')) })
        expect(ownRow(20)).not.toBeNull()
        const current = useAppStore.getState().sessions.find(session => session.id === child.id)!
        child = { ...current, messageCount: 2, messagesDigest: 'owned-shrunk-v2', userMessageIndices: [0] }
        await act(async () => useAppStore.setState({ sessions: [parent, child] }))
        expect(ownRow(20)).not.toBeNull()
        expect(host.textContent).toContain('The retained reading position is no longer in the current history.')
        expect(held).toHaveLength(1) // An out-of-range target is never sent.
        const latest = [...host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent === 'Show latest')!
        expect(latest).toBeDefined()
        await act(async () => latest.click())
        expect(held).toHaveLength(2)
        expect(held[1].envelope.payload).toMatchObject({ targetMessageIndex: 1, expectedMessagesDigest: 'owned-shrunk-v2' })
        await act(async () => held[1].finish('success'))
        expect(ownRow(20)).toBeNull()
        expect(ownRow(1)?.textContent).toContain('Canonical child history row 1')
        expectNoInputOrGlobalNavigation()
    })
})

describe('current rail navigation after retained-range refusal', () => {
    it('revalidates an overlapping old row against the current page digest before clearing its error', async () => {
        await commitFirstScopedWindow()
        await changeOnlyCatalogue('digest')
        expect(held).toHaveLength(2)
        await act(async () => held[1].finish('error'))
        expect(ownRow(0)?.textContent).toContain(targetText)
        const notice = 'The child history page was refused or changed.'
        expect(host.textContent).toContain(notice)
        const rail = host.querySelector<HTMLElement>('[role="slider"][aria-label="Navigate user messages"]')!
        await act(async () => rail.dispatchEvent(new MouseEvent('click', { bubbles: true, clientY: 20 })))
        expect(held).toHaveLength(3)
        expect(held[2].envelope.payload).toMatchObject({ targetMessageIndex: 0, expectedMessagesDigest: 'owned-child-digest-v2', limit: 50 })
        expect(host.textContent).toContain(notice)
        await act(async () => held[2].finish('success'))
        expect(host.textContent).not.toContain(notice)
        expect(ownRow(0)?.textContent).toContain(targetText)
        expectNoInputOrGlobalNavigation()
    })
})

describe('shared accepted pages and explicit import boundaries', () => {
    it('lets concurrent panes use the same guarded adoption after the first caller is superseded', async () => {
        await render()
        await act(async () => root.render(<div />))
        const realCef = window.cefQuery!
        const tails: { done: boolean; finish: () => void }[] = []
        window.cefQuery = args => {
            const envelope = JSON.parse(args.request) as Envelope
            if (envelope.action === 'getChatMessages' && envelope.payload?.chatId === child.id && envelope.payload?.targetMessageIndex === undefined) {
                envelopes.push(envelope)
                const pending = { done: false, finish: () => {
                    if (pending.done) return
                    pending.done = true; settledEnvelopes += 1
                    args.onSuccess(JSON.stringify({ ok: true, data: { chatId: child.id, messagesDigest: child.messagesDigest,
                        totalCount: 1200, startIndex: 1150, userMessageIndices: indices(1200), messages: wire(1150, 1200) } }))
                } }
                tails.push(pending)
            } else realCef(args)
        }
        try {
            await act(async () => root.render(<>
                <NativeConversation parent={parent} chatId={child.id} navigationKey={navigationKey + '-one'} autoFocus={false} onBack={() => {}} />
                <NativeConversation parent={parent} chatId={child.id} navigationKey={navigationKey + '-two'} autoFocus={false} onBack={() => {}} />
            </>))
            expect(tails).toHaveLength(2)
            await act(async () => tails[0].finish())
            expect(tails[1].done).toBe(false)
            await act(async () => tails[1].finish())
            const state = useAppStore.getState()
            expect(state.chatHistoryPageBySessionId[child.id].messages).toBe(state.messages[child.id])
            const rails = [...host.querySelectorAll<HTMLElement>('[role="slider"][aria-label="Navigate user messages"]')]
            expect(rails).toHaveLength(2)
            for (const rail of rails) await act(async () => rail.dispatchEvent(new MouseEvent('click', { bubbles: true, clientY: 20 })))
            expect(held).toHaveLength(2)
            expect(held.every(item => item.envelope.payload?.targetMessageIndex === 0)).toBe(true)
            await act(async () => held.forEach(item => item.finish('success')))
            expect(host.querySelectorAll('[data-message-index="0"]')).toHaveLength(2)
            expect(useAppStore.getState()).toBe(state)
            expectNoInputOrGlobalNavigation()
        } finally { await act(async () => tails.forEach(item => item.finish())) }
    })

    it('does not adopt another reader publication while an explicit native reimport is still pending', async () => {
        const oldId = child.id
        parent = { ...parent, providerId: 'claude-cli' }
        child = { ...child, id: `claude-agent-held-import-${serial}`, providerId: 'claude-cli' }
        useAppStore.setState(state => ({ sessions: [parent, child],
            messages: { ...state.messages, [child.id]: state.messages[oldId].map(row => ({ ...row, sessionId: child.id })) },
            historyStartIndexBySessionId: { ...state.historyStartIndexBySessionId, [child.id]: 1150 } }))
        await commitFirstScopedWindow()
        let finishImport!: (id: string) => void
        const imported = new Promise<string>(resolve => { finishImport = resolve })
        const reimport = vi.spyOn(useAppStore.getState(), 'openSubAgentSession').mockReturnValue(imported)
        try {
            const refresh = [...host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.getAttribute('aria-label') === 'Refresh conversation')!
            await act(async () => refresh.click())
            expect(reimport).toHaveBeenCalledTimes(1)
            await act(async () => { await previous.loadSessionMessages(child.id, true) })
            expect(held).toHaveLength(1) // Publication before the actual manual read grants no new epoch.
            expect(ownRow(0)?.textContent).toContain(targetText)
            await act(async () => finishImport(child.id))
            expect(held).toHaveLength(2)
            expect(held[1].envelope.payload).toMatchObject({ targetMessageIndex: 0, expectedMessagesDigest: 'owned-child-digest-v1' })
            await act(async () => held[1].finish('success'))
            expect(ownRow(0)?.textContent).toContain(targetText)
            expectNoInputOrGlobalNavigation()
        } finally { await act(async () => finishImport(child.id)) }
    })
})

// TESTONLY shared-writer controls; all existing source above is an exact prefix.
const writerPageDigest = 'accepted-page-distinct-from-summary'
function installSharedWriterBridge() {
    const original = window.cefQuery!
    window.cefQuery = args => {
        const envelope = JSON.parse(args.request) as Envelope
        if (envelope.action !== 'getChatMessages' || envelope.payload?.chatId !== child.id) { original(args); return }
        envelopes.push(envelope)
        const payload = envelope.payload!
        const total = child.messageCount!
        if (payload.targetMessageIndex !== undefined) {
            const end = Math.min(total, Number(payload.before)), start = Math.max(0, end - 50)
            const item: HeldRead = { envelope, done: false, finish: outcome => {
                if (item.done) return
                item.done = true; settledEnvelopes += 1
                args.onSuccess(JSON.stringify(outcome === 'error' ? { ok: false, error: staleError } : { ok: true, data: {
                    chatId: child.id, messagesDigest: writerPageDigest, startIndex: start, totalCount: total,
                    userMessageIndices: indices(total), messages: wire(start, end),
                } }))
            } }
            held.push(item)
            return
        }
        settledEnvelopes += 1
        const older = payload.before !== undefined
        const end = older ? Number(payload.before) : total
        const start = Math.max(0, end - (older ? 100 : 50))
        args.onSuccess(JSON.stringify({ ok: true, data: { chatId: child.id, messagesDigest: writerPageDigest,
            startIndex: start, totalCount: total, userMessageIndices: indices(total), messages: wire(start, end) } }))
    }
}
async function mintRealSharedWriterTail() {
    installSharedWriterBridge()
    expect(useAppStore.getState().loadSessionMessages).toBe(previous.loadSessionMessages)
    expect(vi.isMockFunction(useAppStore.getState().loadSessionMessages)).toBe(false)
    await render()
    const state = useAppStore.getState(), page = state.chatHistoryPageBySessionId[child.id]
    expect(page).toBeDefined()
    expect(page.messagesDigest).toBe(writerPageDigest)
    expect(page.messagesDigest).not.toBe(state.sessions.find(session => session.id === child.id)!.messagesDigest)
    expect(page.messages).toBe(state.messages[child.id]); expect(page.startIndex).toBe(1150)
    expect(ownRow(1198)?.textContent).toContain('Canonical child history row 1198')
    expect(held).toHaveLength(0)
    return page
}
const ordinaryTailReadCount = () => envelopes.filter(envelope => envelope.action === 'getChatMessages' &&
    envelope.payload?.chatId === child.id && envelope.payload?.before === undefined).length
async function requestSharedWriterRailZero() {
    const rail = host.querySelector<HTMLElement>('[role="slider"][aria-label="Navigate user messages"]')!
    expect(rail).not.toBeNull()
    const count = held.length
    await act(async () => rail.dispatchEvent(new MouseEvent('click', { bubbles: true, clientY: 20 })))
    expect(held).toHaveLength(count + 1)
    const pending = held[count]
    expect(pending.envelope.payload).toEqual({ chatId: child.id, targetMessageIndex: 0, expectedMessagesDigest: writerPageDigest,
        before: 26, limit: 50, deferToolCallContent: true })
    expect(pending.done).toBe(false)
    return pending
}

describe('real shared-history writers versus passive local page authority, authored UNRUN', () => {
    it('keeps a minted local tail readable after real older-page loading and navigates with its page digest without changing the shared range', async () => {
        const page = await mintRealSharedWriterTail()
        const sessionBefore = useAppStore.getState().sessions.find(session => session.id === child.id)!
        const parentRows = useAppStore.getState().messages[parent.id]
        const tails = ordinaryTailReadCount()
        expect(useAppStore.getState().loadOlderSessionMessages).toBe(previous.loadOlderSessionMessages)
        let loaded: boolean | undefined
        await act(async () => { loaded = await previous.loadOlderSessionMessages(child.id) })
        expect(loaded).toBe(true)
        const shared = useAppStore.getState().messages[child.id]
        expect(shared).not.toBe(page.messages)
        expect(useAppStore.getState().historyStartIndexBySessionId[child.id]).toBe(1050)
        expect(shared).toHaveLength(150)
        expect(useAppStore.getState().sessions.find(session => session.id === child.id)).toBe(sessionBefore)
        expect(ordinaryTailReadCount()).toBe(tails)
        expect(ownRow(1198)?.textContent).toContain('Canonical child history row 1198')
        const pending = await requestSharedWriterRailZero()
        await act(async () => pending.finish('success'))
        expect(ownRow(0)?.textContent).toContain(targetText)
        expect(useAppStore.getState().messages[child.id]).toBe(shared)
        expect(useAppStore.getState().historyStartIndexBySessionId[child.id]).toBe(1050)
        expect(useAppStore.getState().sessions.find(session => session.id === child.id)).toBe(sessionBefore)
        expect(useAppStore.getState().messages[parent.id]).toBe(parentRows)
        expect(ordinaryTailReadCount()).toBe(tails)
        expectNoInputOrGlobalNavigation()
    })

    it.each(['success', 'error'] as const)('retires pending target %s after real unload removes its published page and shared rows', async outcome => {
        await mintRealSharedWriterTail()
        const pending = await requestSharedWriterRailZero()
        await act(async () => previous.unloadSessionMessages(child.id))
        const state = useAppStore.getState()
        expect(state.chatHistoryPageBySessionId[child.id]).toBeUndefined()
        expect(state.messages[child.id]).toBeUndefined()
        const beforeHtml = host.innerHTML, beforeRows = state.messages, beforeErrors = state.chatHistoryErrorBySessionId
        const requests = envelopes.length
        await act(async () => pending.finish(outcome))
        expect(host.innerHTML).toBe(beforeHtml)
        expect(ownRow(0)).toBeNull(); expect(host.textContent).not.toContain(staleError)
        expect(useAppStore.getState().messages).toBe(beforeRows)
        expect(useAppStore.getState().chatHistoryErrorBySessionId).toBe(beforeErrors)
        expect(useAppStore.getState().chatHistoryPageBySessionId[child.id]).toBeUndefined()
        expect(envelopes).toHaveLength(requests)
        expectNoInputOrGlobalNavigation()
    })
})

// TESTONLY: a held restore may not overwrite a newer committed reading position.
describe('scroll intent changed during retained-range restore, authored UNRUN', () => {
    it('preserves the newer visible row and pixel offset when a pending restore page contains that row', async () => {
        await commitFirstScopedWindow()
        const parentRows = useAppStore.getState().messages[parent.id]
        const snapshot = () => {
            const region = host.querySelector<HTMLElement>('[aria-label="Native conversation messages"]')!
            expect(region).not.toBeNull()
            const bounds = region.getBoundingClientRect()
            const visible = [...region.querySelectorAll<HTMLElement>('[data-message-index]')].find(row => {
                const box = row.getBoundingClientRect()
                return box.height > 0 && box.bottom > bounds.top && box.top < bounds.bottom
            })
            expect(visible, 'The existing geometry must expose a genuinely visible retained row').toBeDefined()
            return { index: Number(visible!.dataset.messageIndex), text: visible!.textContent!,
                offset: visible!.getBoundingClientRect().top - bounds.top }
        }
        const original = snapshot()
        await changeOnlyCatalogue('digest')
        expect(held).toHaveLength(2)
        const restore = held[1]
        expect(restore.done).toBe(false)
        expect(restore.envelope.payload).toEqual({ chatId: child.id, targetMessageIndex: 0,
            expectedMessagesDigest: 'owned-child-digest-v2', limit: 50, before: 26, deferToolCallContent: true })
        expect(ownRow(0)?.textContent).toContain(targetText)
        const region = host.querySelector<HTMLElement>('[aria-label="Native conversation messages"]')!
        const range = region.scrollHeight - region.clientHeight
        expect(range).toBeGreaterThan(650)
        await act(async () => {
            region.scrollTop = Math.floor(range / 2) + 13
            region.dispatchEvent(new Event('scroll', { bubbles: true }))
        })
        const latest = snapshot()
        expect(latest.index).not.toBe(original.index)
        expect(latest.offset).not.toBe(original.offset)
        // This fixture's genuine restore reply covers [0,26), so positive preservation is possible.
        expect(latest.index).toBeGreaterThanOrEqual(0); expect(latest.index).toBeLessThan(26)
        expect(latest.text).toContain(`Canonical child history row ${latest.index}`)
        expect(held).toHaveLength(2); expect(restore.done).toBe(false)
        expect(host.textContent).not.toContain('This child catalogue is unavailable')
        const sharedRows = useAppStore.getState().messages[child.id]
        const sharedStart = useAppStore.getState().historyStartIndexBySessionId[child.id]
        const requestCount = envelopes.length
        await act(async () => restore.finish('success'))
        expect(snapshot()).toEqual(latest)
        expect(envelopes).toHaveLength(requestCount)
        expect(held).toHaveLength(2)
        expect(useAppStore.getState().messages[child.id]).toBe(sharedRows)
        expect(useAppStore.getState().historyStartIndexBySessionId[child.id]).toBe(sharedStart)
        expect(useAppStore.getState().messages[parent.id]).toBe(parentRows)
        expect(host.textContent).not.toContain('The child history page was refused or changed.')
        expectNoInputOrGlobalNavigation()
    })
})

// TESTONLY browser-like asynchronous scroll delivery for genuine programmatic writes.
function queueActualProgrammaticScrolls() {
    const setter = Object.getOwnPropertyDescriptor(Element.prototype, 'scrollTop')?.set
    expect(setter).toBeDefined(); expect(vi.isMockFunction(setter)).toBe(true)
    const mockedSetter = vi.mocked(setter!)
    const captured = mockedSetter.getMockImplementation()
    expect(captured).toBeTypeOf('function')
    const events: { element: Element; delivered: boolean }[] = []
    mockedSetter.mockImplementation(function (this: Element, value: number) {
        const before = this.scrollTop
        captured!.call(this, value)
        if (!this.classList.contains('uam-subagent-transcript') || this.scrollTop === before) return
        const event = { element: this, delivered: false }; events.push(event)
        requestAnimationFrame(() => {
            event.delivered = true
            if (this.isConnected) this.dispatchEvent(new Event('scroll', { bubbles: true }))
        })
    })
    return events
}
async function drainProgrammaticScrollFrames() {
    for (let pass = 0; frames.size && pass < 20; ++pass) {
        const callbacks = [...frames.values()]; frames.clear()
        await act(async () => callbacks.forEach(callback => callback(pass * 16)))
    }
    expect(frames.size, 'The existing rAF geometry must settle without runtime timer advancement').toBe(0)
}
function reopenVisibleVisit() {
    const region = host.querySelector<HTMLElement>('[aria-label="Native conversation messages"]')!
    expect(region).not.toBeNull()
    const bounds = region.getBoundingClientRect()
    const row = [...region.querySelectorAll<HTMLElement>('[data-message-index]')].find(element => {
        const box = element.getBoundingClientRect()
        return box.height > 0 && box.bottom > bounds.top && box.top < bounds.bottom
    })
    expect(row).toBeDefined()
    return { index: Number(row!.dataset.messageIndex), text: row!.textContent!, offset: row!.getBoundingClientRect().top - bounds.top }
}
async function establishAsyncSavedZero(events: ReturnType<typeof queueActualProgrammaticScrolls>) {
    await commitFirstScopedWindow()
    await drainProgrammaticScrollFrames()
    const region = host.querySelector<HTMLElement>('[aria-label="Native conversation messages"]')!
    await act(async () => { region.scrollTop = 17 })
    expect(events.some(event => !event.delivered)).toBe(true)
    await drainProgrammaticScrollFrames()
    expect(events.length).toBeGreaterThan(0)
    expect(events.every(event => event.delivered)).toBe(true)
    const saved = reopenVisibleVisit()
    expect(saved.index).toBe(0); expect(saved.text).toContain(targetText); expect(saved.offset).toBe(-17)
    expectNoInputOrGlobalNavigation()
    return saved
}

describe('asynchronous programmed scroll during actual root reopen, authored UNRUN', () => {
    it('does not let a queued initial-bottom scroll replace the saved distant target0 visit before restore replies', async () => {
        const events = queueActualProgrammaticScrolls()
        const saved = await establishAsyncSavedZero(events)
        await act(async () => root.unmount())
        root = createRoot(host)
        const before = held.length, eventStart = events.length
        await render()
        // The cached attestation starts one restore; the ordinary reopened read then publishes
        // a replacement attestation, retires that first request and restores the same saved visit.
        expect(held).toHaveLength(before + 2)
        const retired = held[before]
        const restore = held.at(-1)!
        expect(restore.envelope.payload).toMatchObject({ targetMessageIndex: 0, expectedMessagesDigest: 'owned-child-digest-v1', before: 26 })
        expect(restore.done).toBe(false)
        // Every genuine reopen write is delivered before the reply. A fixed implementation
        // may correctly skip that write; establishment above proves the wrapper is not inert.
        await drainProgrammaticScrollFrames()
        expect(events.slice(eventStart).every(event => event.delivered)).toBe(true)
        expect(restore.done).toBe(false)
        await act(async () => retired.finish('success'))
        expect(restore.done).toBe(false)
        await act(async () => restore.finish('success'))
        await drainProgrammaticScrollFrames()
        expect(reopenVisibleVisit()).toEqual(saved)
        expect(host.textContent).not.toContain('This child catalogue is unavailable')
        expectNoInputOrGlobalNavigation()
    })

    it('keeps the closed visit through changed-summary reopen when initial-bottom events precede held fresh hydration', async () => {
        const events = queueActualProgrammaticScrolls()
        const saved = await establishAsyncSavedZero(events)
        await act(async () => root.unmount())
        const beforeClosed = envelopes.length
        await changeOnlyCatalogue('digest')
        expect(envelopes).toHaveLength(beforeClosed)
        const original = window.cefQuery!
        window.cefQuery = args => {
            const envelope = JSON.parse(args.request) as Envelope
            if (envelope.action !== 'getChatMessages' || envelope.payload?.chatId !== child.id || envelope.payload?.targetMessageIndex !== undefined) { original(args); return }
            envelopes.push(envelope)
            const captured = useAppStore.getState().sessions.find(session => session.id === child.id)!
            const item: HeldRead = { envelope, done: false, finish: outcome => {
                if (item.done) return
                item.done = true; settledEnvelopes += 1
                args.onSuccess(JSON.stringify(outcome === 'error' ? { ok: false, error: staleError } : { ok: true, data: {
                    chatId: child.id, messagesDigest: captured.messagesDigest, startIndex: 1150, totalCount: 1200,
                    userMessageIndices: indices(1200), messages: wire(1150, 1200),
                } }))
            } }
            held.push(item)
        }
        root = createRoot(host)
        const count = held.length, eventStart = events.length
        await render()
        expect(held).toHaveLength(count + 1)
        const hydration = held[count]
        expect(hydration.envelope.payload).toEqual({ chatId: child.id, limit: 50, deferToolCallContent: true, messagesDigest: 'owned-child-digest-v1' })
        expect(hydration.done).toBe(false)
        await drainProgrammaticScrollFrames()
        expect(events.slice(eventStart).every(event => event.delivered)).toBe(true)
        expect(hydration.done).toBe(false)
        await act(async () => hydration.finish('success'))
        expect(held).toHaveLength(count + 2)
        const restore = held[count + 1]
        expect(restore.envelope.payload).toMatchObject({ targetMessageIndex: 0, expectedMessagesDigest: 'owned-child-digest-v2', before: 26 })
        expect(restore.done).toBe(false)
        await drainProgrammaticScrollFrames()
        await act(async () => restore.finish('success'))
        await drainProgrammaticScrollFrames()
        expect(reopenVisibleVisit()).toEqual(saved)
        expect(host.textContent).not.toContain('This child catalogue is unavailable')
        expectNoInputOrGlobalNavigation()
    })
})
