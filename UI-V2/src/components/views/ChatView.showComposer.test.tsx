import { act, useState } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ChatView } from './ChatView'
import { MainPanel } from '../layout/MainPanel'
import { useAppStore } from '../../store/useAppStore'
import type { Session } from '../../types/session'
import type { Message } from '../../types/message'
import { fallbackProviderForId } from '../../utils/providerMetadata'
import { acpBindingFromCppChat } from '../../store/cpp/reconcile'
import { sanitizeCppAcpSession } from '../../store/cpp/sanitizers'
import { captureContextReference } from '../../utils/contextReferences'
import type { AcpPromptPresentationObserver } from '../../store/storeTypes'
import { readChatComposerDraft, removeComposerDrafts, writeChatComposerDraft } from '../../utils/composerDraftStorage'
import { resetOutgoingPresentation } from '../../utils/outgoingMessagePresentation'
import { focusActiveInput } from '../../utils/activeInputFocus'
import { canExecuteNativeSubagent, clearSubagentView, openSubagentView, rememberSubagentParent, subagentRouteFor } from '../../utils/subagentNavigation'
import { chatGridLeaves, defaultChatGridLayout, readChatGridLayout, setChatInLeaf, writeChatGridLayout } from '../../utils/chatGridStorage'

// Private authored controls only. Requires the c408 ChatView proposal plus frozen R2
// and its separate MainPanel dependent hunk. None of these cases has been executed.
;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const parent: Session = {
    id: 'composer-contract-parent', name: 'Parent task', providerId: 'codex-cli', viewMode: 'chat', folderId: null,
    createdAt: new Date('2026-10-22T09:00:00Z'), updatedAt: new Date('2026-10-22T09:00:00Z'),
    messagesDigest: 'parent-digest', userMessageIndices: [0], messageCount: 2,
}
const child: Session = { ...parent, id: 'composer-contract-child', name: 'Child task', messagesDigest: 'child-digest' }
const history = (session: Session): Message[] => [
    { id: `${session.id}-user`, sessionId: session.id, role: 'user', content: `${session.name} request`, createdAt: session.createdAt },
    { id: `${session.id}-answer`, sessionId: session.id, role: 'assistant', content: `**${session.name}** answer\n\n\`\`\`ts\nconst value = 1\n\`\`\``, createdAt: session.updatedAt },
]
let host: HTMLDivElement, root: Root, previous: ReturnType<typeof useAppStore.getState>, previousCef: Window['cefQuery']
let frames: Map<number, FrameRequestCallback>, frameSerial: number
const flushFrame = () => { const pending = [...frames.values()]; frames.clear(); pending.forEach(callback => callback(16)) }
const field = () => host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')
const button = (name: string) => [...host.querySelectorAll<HTMLButtonElement>('button')]
    .find(item => (item.getAttribute('aria-label') || item.textContent?.trim()) === name)

function binding(session: Session, changes: Record<string, unknown> = {}) {
    return acpBindingFromCppChat({ id: session.id, title: session.name, folderId: 'default', providerId: 'codex-cli',
        createdAt: session.createdAt.toISOString(), updatedAt: session.updatedAt.toISOString(),
        acpSession: sanitizeCppAcpSession({ running: true, processing: false, lifecycleState: 'ready',
            providerId: 'codex-cli', protocolKind: 'codex-app-server', sessionId: `native-${session.id}`,
            threadId: `native-${session.id}`, turnEvents: [], toolCalls: [], pendingPermission: null, pendingUserInput: null,
            queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [], ...changes }),
    }, undefined)
}

beforeEach(() => {
    previous = useAppStore.getState(); previousCef = window.cefQuery
    const values = new Map<string, string>()
    vi.stubGlobal('localStorage', { getItem: (key: string) => values.get(key) ?? null,
        setItem: (key: string, value: string) => values.set(key, value), removeItem: (key: string) => values.delete(key) })
    frames = new Map(); frameSerial = 0
    vi.stubGlobal('requestAnimationFrame', (callback: FrameRequestCallback) => { frames.set(++frameSerial, callback); return frameSerial })
    vi.stubGlobal('cancelAnimationFrame', (id: number) => frames.delete(id))
    vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
    vi.stubGlobal('IntersectionObserver', class { observe() {} unobserve() {} disconnect() {} })
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify({ ok: true, data: {} }))
    // Draft memory outlives a localStorage stub; isolate fixture owners without changing production retention.
    removeComposerDrafts([parent.id, child.id])
    resetOutgoingPresentation()
    for (const leaf of chatGridLeaves(readChatGridLayout().root)) clearSubagentView(leaf.id)
    writeChatGridLayout(setChatInLeaf(defaultChatGridLayout, parent.id, defaultChatGridLayout.activeLeafId))
    useAppStore.setState({ sessions: [parent, child], providers: [fallbackProviderForId('codex-cli')],
        folders: [], resourceCollections: [], activeSessionId: parent.id, lastAppliedStateRevision: 1,
        messages: { [parent.id]: history(parent), [child.id]: history(child) },
        acpBindingBySessionId: { [parent.id]: binding(parent), [child.id]: binding(child) }, cliBindingBySessionId: {},
        historyStartIndexBySessionId: { [parent.id]: 0, [child.id]: 0 }, chatHistoryErrorBySessionId: {}, chatSearchNavigation: null,
        goalsByChatId: {}, activeGoalIdByChatId: {}, markdownStoreAttachedBySessionId: {},
        sendAcpPrompt: vi.fn().mockResolvedValue(true), startAcpSession: vi.fn().mockResolvedValue(true),
        loadSessionMessages: vi.fn().mockResolvedValue(undefined), unloadSessionMessages: vi.fn(),
        getVcsCommitStatus: vi.fn().mockResolvedValue(null), discoverProviderModels: vi.fn().mockResolvedValue(undefined) })
    host = document.createElement('div'); document.body.append(host); root = createRoot(host)
})

afterEach(() => {
    act(() => root.unmount()); host.remove()
    for (const leaf of chatGridLeaves(readChatGridLayout().root)) clearSubagentView(leaf.id)
    window.cefQuery = previousCef; useAppStore.setState(previous, true)
    vi.unstubAllGlobals(); vi.restoreAllMocks()
})

describe('showComposer presentation contract, authored UNRUN', () => {
    it('keeps omitted and explicit true DOM equivalent and retains the ordinary Send action', async () => {
        writeChatComposerDraft(parent.id, { text: 'Normal parent draft', attachments: [] })
        await act(async () => root.render(<ChatView session={parent} />))
        const before = host.innerHTML
        const textarea = field()
        expect(button('Send prompt')?.disabled).toBe(false)
        await act(async () => root.render(<ChatView session={parent} showComposer />))
        expect(host.innerHTML).toBe(before)
        expect(field()).toBe(textarea)
        expect(field()?.value).toBe('Normal parent draft')
    })

    it('removes the standing dock and Send without replacing the canonical transcript, code or rail', async () => {
        writeChatComposerDraft(child.id, { text: 'Unsent child draft', attachments: [] })
        await act(async () => root.render(<ChatView session={child} showComposer={false} />))
        expect(host.querySelector('.uam-composer-dock')).toBeNull()
        expect(field()).toBeNull(); expect(button('Send prompt')).toBeUndefined()
        expect(host.querySelector('.uam-chat-transcript .uam-message-list')).not.toBeNull()
        expect(host.querySelector('.uam-message-list code')?.textContent).toContain('const value = 1')
        expect(host.querySelector('[aria-label="Navigate user messages"]')).not.toBeNull()
        expect(host.querySelector('[style*="--composer-dock-h"]')?.getAttribute('style')).toContain('0px')
        expect(readChatComposerDraft(child.id).text).toBe('Unsent child draft')
    })

    it.each(['Enter', 'Control+Enter', 'Meta+Enter'])('blocks %s and form submission without taking focus or consuming child resources', async key => {
        const reference = captureContextReference({ id: 'quoted-child-context', kind: 'message', sourceChatId: child.id,
            sourceMessageIndex: 0, sourceMessageId: `${child.id}-user`, executionHostId: 'local', workspaceDirectory: '',
            capturedAt: '2026-10-22T09:01:00Z', label: 'Child quotation', text: 'Keep this quoted context' })!
        writeChatComposerDraft(child.id, { text: 'Never submit this hidden draft', attachments: [
            { id: 'kept-file', name: 'kept.txt', type: 'file', mimeType: 'text/plain', size: 4, path: '/tmp/kept.txt' },
        ], references: [reference] })
        const saved = readChatComposerDraft(child.id)
        const sentinel = document.createElement('button'); document.body.append(sentinel); sentinel.focus()
        host.className = 'uam-chat-pane'; host.dataset.focused = 'true'
        try {
            await act(async () => root.render(<ChatView session={child} showComposer={false} />))
            expect(focusActiveInput()).toBe(false)
            const send = useAppStore.getState().sendAcpPrompt
            await act(async () => {
                const event = { key: 'Enter', bubbles: true, cancelable: true, ctrlKey: key === 'Control+Enter', metaKey: key === 'Meta+Enter' }
                window.dispatchEvent(new KeyboardEvent('keydown', event))
                document.dispatchEvent(new KeyboardEvent('keydown', event))
                host.dispatchEvent(new KeyboardEvent('keydown', event))
                host.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
                window.dispatchEvent(new CustomEvent('uam-dictation', { detail: { type: 'dictation', event: 'final', text: 'Do not submit', dictationId: 'foreign-dictation' } }))
            })
            expect(document.activeElement).toBe(sentinel)
            expect(send).not.toHaveBeenCalled()
            expect(useAppStore.getState().startAcpSession).not.toHaveBeenCalled()
            expect(readChatComposerDraft(child.id)).toEqual(saved)
        } finally { sentinel.remove(); host.className = ''; delete host.dataset.focused }
    })

    it('preserves the current transcript and off-tail scroll when the standing dock is hidden then restored', async () => {
        writeChatComposerDraft(child.id, { text: 'Still my child draft', attachments: [] })
        await act(async () => root.render(<ChatView session={child} navigationKey="same-child-pane" />))
        const transcript = host.querySelector<HTMLElement>('.uam-chat-transcript')!
        const message = transcript.querySelector('[data-message-index="0"]')
        Object.defineProperties(transcript, { scrollHeight: { configurable: true, value: 2000 }, clientHeight: { configurable: true, value: 300 } })
        transcript.scrollTop = 240
        act(() => { transcript.dispatchEvent(new Event('scroll', { bubbles: true })); flushFrame() })
        await act(async () => root.render(<ChatView session={child} navigationKey="same-child-pane" showComposer={false} />))
        expect(host.querySelector('.uam-chat-transcript')).toBe(transcript)
        expect(host.querySelector('[data-message-index="0"]')).toBe(message)
        expect(transcript.scrollTop).toBe(240)
        await act(async () => root.render(<ChatView session={child} navigationKey="same-child-pane" showComposer />))
        expect(transcript.scrollTop).toBe(240)
        expect(field()?.value).toBe('Still my child draft')
    })

    it.each([false, true])('keeps explicit unsent recovery and its existing permissions (importedReadOnly=%s)', async readOnly => {
        const recoveryChild = { ...child, importedReadOnly: readOnly }
        const row: Message = { ...history(child)[0], content: 'Unconfirmed child input', acpPromptNotSent: true, interrupted: true }
        const retry = vi.fn().mockResolvedValue({ ok: false, error: 'Delivery remains unconfirmed.' })
        useAppStore.setState({ sessions: [parent, recoveryChild], activeSessionId: child.id, retryFailedMessage: retry,
            messages: { [parent.id]: history(parent), [child.id]: [row] },
            acpBindingBySessionId: { [child.id]: binding(child, { unsentMessageIndices: [0], recoverableUnsentMessageIndices: [0] }) } })
        writeChatComposerDraft(child.id, { text: 'Independent hidden child draft', attachments: [] })
        await act(async () => root.render(<ChatView session={recoveryChild} showComposer={false} />))
        expect(host.textContent).toContain('Unconfirmed child input')
        expect(field()).toBeNull(); expect(button('Send prompt')).toBeUndefined()
        const retryButton = button('Retry message')!
        expect(retryButton).toBeDefined(); expect(retryButton.disabled).toBe(readOnly)
        await act(async () => retryButton.click())
        if (readOnly) expect(retry).not.toHaveBeenCalled()
        else {
            expect(retry).toHaveBeenCalledWith(child.id, 0)
            expect(host.textContent).toContain('Delivery remains unconfirmed.')
            await act(async () => button('Edit unsent message')!.click())
            const editor = host.querySelector<HTMLTextAreaElement>('[aria-label="Edit message"]')!
            expect(editor.value).toBe('Unconfirmed child input')
            expect(document.activeElement).toBe(editor)
            expect(button('Save changes')).toBeDefined()
            expect(button('Send prompt')).toBeUndefined()
            await act(async () => button('Cancel')!.click())
            expect(field()).toBeNull()
        }
        expect(readChatComposerDraft(child.id).text).toBe('Independent hidden child draft')
        expect(useAppStore.getState().sendAcpPrompt).not.toHaveBeenCalled()
    })

    it('keeps held-queue recovery and runtime error content outside the hidden standing editor', async () => {
        useAppStore.setState({ activeSessionId: child.id, acpBindingBySessionId: { [child.id]: binding(child, {
            lastError: 'Child delivery needs attention', queueHeld: true,
            queuedPrompts: [{ id: 'own-held', revision: 1, text: 'Existing held child input', attachments: [], markdownStoreFiles: [], goalMode: false, goalId: '' }],
        }) } })
        await act(async () => root.render(<ChatView session={child} showComposer={false} />))
        expect(host.textContent).toContain('Existing held child input')
        expect(host.textContent).toContain('Child delivery needs attention')
        expect(button('Copy error')).toBeDefined()
        expect(field()).toBeNull(); expect(button('Send prompt')).toBeUndefined()
        expect(useAppStore.getState().sendAcpPrompt).not.toHaveBeenCalled()
    })

    it('returns through the actual native-child Back control with independent drafts, parent scroll and parent focus intact', async () => {
        rememberSubagentParent(parent, child.id)
        writeChatComposerDraft(parent.id, { text: 'Parent draft before Open', attachments: [] })
        writeChatComposerDraft(child.id, { text: 'Independent native child draft', attachments: [] })
        await act(async () => root.render(<MainPanel />))
        const parentPane = host.querySelector<HTMLElement>(`[data-testid="chat-pane-${parent.id}"]`)!
        const parentScroll = parentPane.querySelector<HTMLElement>('.uam-chat-transcript')!
        Object.defineProperties(parentScroll, { scrollHeight: { configurable: true, value: 2000 }, clientHeight: { configurable: true, value: 300 } })
        parentScroll.scrollTop = 220
        act(() => { parentScroll.dispatchEvent(new Event('scroll', { bubbles: true })); flushFrame() })
        await act(async () => openSubagentView(parent, { kind: 'native', chatId: child.id }))
        const childPane = host.querySelector<HTMLElement>(`[data-testid="subagent-pane-${parent.id}"]`)!
        expect(childPane.querySelector('.uam-message-list')?.textContent).toContain('Child task answer')
        expect(childPane.querySelector('.uam-message-list')?.textContent).not.toContain('Parent task answer')
        expect(childPane.querySelector('.uam-composer-textarea')).toBeNull()
        expect(childPane.querySelector('[aria-label="Send prompt"]')).toBeNull()
        const back = button('Back to parent')!
        back.focus()
        await act(async () => {
            window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', ctrlKey: true, bubbles: true }))
            childPane.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
        })
        expect(document.activeElement).toBe(back)
        expect(useAppStore.getState().sendAcpPrompt).not.toHaveBeenCalled()
        expect(readChatComposerDraft(child.id).text).toBe('Independent native child draft')
        await act(async () => { back.click(); flushFrame() })
        const returned = host.querySelector<HTMLElement>(`[data-testid="chat-pane-${parent.id}"]`)!
        const restored = returned.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
        expect(restored.value).toBe('Parent draft before Open')
        expect(returned.querySelector<HTMLElement>('.uam-chat-transcript')!.scrollTop).toBe(220)
        expect(document.activeElement).toBe(restored)
        expect(readChatComposerDraft(child.id).text).toBe('Independent native child draft')
    })

    it.each(['codex-cli', 'opencode-cli', 'antigravity-cli'])('fixture-only explicit native follow-up opt-in preserves the five-argument send observer: %s', async providerId => {
        // This fixture exercises the approved prop, NOT an existing product affordance.
        // This base-hook fixture remains distinct from the actual MainPanel route controls below.
        const ownChild = { ...child, providerId }
        const send = vi.fn().mockResolvedValue(true)
        useAppStore.setState(state => ({ activeSessionId: ownChild.id, sessions: [parent, ownChild],
            providers: state.providers.some(provider => provider.id === providerId) ? state.providers : [...state.providers, fallbackProviderForId(providerId)],
            acpBindingBySessionId: {}, sendAcpPrompt: send }))
        const reference = captureContextReference({ id: 'quoted-child-context', kind: 'message', sourceChatId: ownChild.id,
            sourceMessageIndex: 0, sourceMessageId: `${ownChild.id}-user`, executionHostId: 'local', workspaceDirectory: '',
            capturedAt: '2026-10-22T09:01:00Z', label: 'Child quotation', text: 'Keep this quoted context' })!
        writeChatComposerDraft(ownChild.id, { text: '', attachments: [], references: [reference] })
        const expectedPrompt = 'Explicit child fixture follow-up\n\nReferenced context (quoted snapshot):\n' +
            '{"id":"quoted-child-context","kind":"message","sourceChatId":"composer-contract-child","sourceMessageIndex":0,"sourceMessageId":"composer-contract-child-user","executionHostId":"local","workspaceDirectory":"","capturedAt":"2026-10-22T09:01:00Z","label":"Child quotation","truncated":false,"text":"Keep this quoted context"}'
        function ExplicitOptInFixture() {
            const [open, setOpen] = useState(false)
            return <><button type="button" onClick={() => setOpen(true)}>Open native follow-up fixture</button>
                <ChatView session={ownChild} showComposer={open} showSubagentLineage={false} /></>
        }
        await act(async () => root.render(<ExplicitOptInFixture />))
        expect(field()).toBeNull(); expect(button('Send prompt')).toBeUndefined()
        expect(send).not.toHaveBeenCalled()
        await act(async () => button('Open native follow-up fixture')!.click())
        const editor = field()!
        expect(editor).not.toBeNull()
        act(() => {
            Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(editor, 'Explicit child fixture follow-up')
            editor.dispatchEvent(new Event('input', { bubbles: true }))
        })
        await act(async () => host.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })))
        expect(send).toHaveBeenCalledExactlyOnceWith(ownChild.id, expectedPrompt, [], undefined, expect.any(Function))
        expect(typeof send.mock.calls[0][4]).toBe('function')
        // Merely receiving a presentation observer is not a delivery/write witness.
    })

    it.each(['/goal hidden objective --budget 42', '/compact'])('blocks hidden command %s before goal/compaction/send entry', async draft => {
        const goal = vi.fn().mockResolvedValue({ ok: true, goalId: 'not-authorized-by-hidden-input' })
        useAppStore.setState({ activeSessionId: child.id, setGoal: goal })
        writeChatComposerDraft(child.id, { text: draft, attachments: [] })
        await act(async () => root.render(<ChatView session={child} showComposer={false} />))
        const saved = readChatComposerDraft(child.id)
        const bridge = vi.fn(window.cefQuery!)
        window.cefQuery = bridge
        await act(async () => {
            host.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
            window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', ctrlKey: true, bubbles: true }))
            document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', metaKey: true, bubbles: true }))
        })
        expect(goal).not.toHaveBeenCalled()
        expect(useAppStore.getState().sendAcpPrompt).not.toHaveBeenCalled()
        expect(bridge).not.toHaveBeenCalled()
        expect(readChatComposerDraft(child.id)).toEqual(saved)
        expect(field()).toBeNull(); expect(button('Send prompt')).toBeUndefined()
    })

    it('retires active owned dictation on hide without submitting late output or clearing the staged draft', async () => {
        const requests: Array<{ action: string; payload: Record<string, unknown> }> = []
        window.cefQuery = ({ request, onSuccess }) => {
            requests.push(JSON.parse(request))
            onSuccess(JSON.stringify({ ok: true, data: { started: true, stopped: true } }))
        }
        useAppStore.setState({ activeSessionId: child.id })
        writeChatComposerDraft(child.id, { text: 'Keep my draft', attachments: [] })
        await act(async () => root.render(<ChatView session={child} showComposer />))
        const dictate = host.querySelector<HTMLButtonElement>('[data-dictation-state="idle"]')!
        expect(dictate).not.toBeNull(); expect(dictate.disabled).toBe(false)
        await act(async () => dictate.click())
        const start = requests.find(request => request.action === 'startDictation')!
        expect(start).toBeDefined()
        const dictationId = start.payload.dictationId as string
        await act(async () => {
            window.dispatchEvent(new CustomEvent('uam-dictation', { detail: { type: 'dictation', event: 'interim', text: 'Spoken fragment', dictationId } }))
        })
        // Commit the interim React state before testing persistence and then hiding the dock.
        expect(field()!.value).toBe('Keep my draft Spoken fragment')
        act(() => window.dispatchEvent(new Event('pagehide')))
        const staged = readChatComposerDraft(child.id)
        expect(staged.text).toBe('Keep my draft Spoken fragment')
        await act(async () => root.render(<ChatView session={child} showComposer={false} />))
        expect(requests.filter(request => request.action === 'stopDictation')).toEqual([
            { action: 'stopDictation', payload: { dictationId }, requestId: expect.any(String) },
        ])
        await act(async () => {
            window.dispatchEvent(new CustomEvent('uam-dictation', { detail: { type: 'dictation', event: 'final', text: 'Late fragment must not submit', dictationId } }))
            window.dispatchEvent(new CustomEvent('uam-dictation', { detail: { type: 'dictation', event: 'stopped', dictationId } }))
            window.dispatchEvent(new Event('pagehide'))
            host.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
        })
        expect(useAppStore.getState().sendAcpPrompt).not.toHaveBeenCalled()
        expect(readChatComposerDraft(child.id)).toEqual(staged)
        expect(field()).toBeNull(); expect(button('Send prompt')).toBeUndefined()
    })

    // Actual MainPanel route controls for the dependent native access proposal, all authored UNRUN.
    async function nativeProductRoute(providerId = 'codex-cli') {
        const source = { ...parent, providerId }
        const target = { ...child, providerId, importedReadOnly: false,
            subagentParentChatId: source.id, subagentParentCreatedAt: source.createdAt.toISOString() }
        rememberSubagentParent(source, target.id)
        useAppStore.setState(state => ({ sessions: [source, target], activeSessionId: source.id,
            providers: state.providers.some(provider => provider.id === providerId) ? state.providers : [...state.providers, fallbackProviderForId(providerId)],
            messages: { [source.id]: history(source), [target.id]: history(target) }, acpBindingBySessionId: {},
            stopAcpSession: vi.fn().mockResolvedValue(true), retryFailedMessage: vi.fn().mockResolvedValue({ ok: false }),
            resumeQueuedAcpPrompts: vi.fn().mockResolvedValue(false) }))
        writeChatComposerDraft(source.id, { text: 'Independent parent native-access draft', attachments: [] })
        writeChatGridLayout(setChatInLeaf(defaultChatGridLayout, source.id, defaultChatGridLayout.activeLeafId))
        await act(async () => root.render(<MainPanel />))
        const parentRegion = host.querySelector<HTMLElement>(`[data-testid="chat-pane-${source.id}"] .uam-chat-transcript`)!
        Object.defineProperties(parentRegion, { scrollHeight: { configurable: true, value: 2000 }, clientHeight: { configurable: true, value: 300 } })
        parentRegion.scrollTop = 220
        act(() => { parentRegion.dispatchEvent(new Event('scroll', { bubbles: true })); flushFrame() })
        await act(async () => openSubagentView(source, { kind: 'native', chatId: target.id }))
        return { source, target }
    }

    function actualNativePane() {
        const pane = host.querySelector<HTMLElement>(`[data-testid="subagent-pane-${parent.id}"]`)
        expect(pane).not.toBeNull()
        return pane!
    }
    function scopedAction(pane: Element, name: string) {
        const action = [...pane.querySelectorAll<HTMLButtonElement>('nav button')]
            .find(item => (item.getAttribute('aria-label') || item.textContent?.trim()) === name)
        expect(action, name).toBeDefined()
        return action!
    }
    function enterNative(pane: Element, value: string) {
        const editor = pane.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
        expect(editor).not.toBeNull()
        Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(editor, value)
        editor.dispatchEvent(new Event('input', { bubbles: true }))
    }

    it.each(['codex-cli', 'opencode-cli', 'antigravity-cli'])('actual native route opens its own follow-up from one header and sends once with the exact observer (%s)', async providerId => {
        const { target } = await nativeProductRoute(providerId)
        const pane = actualNativePane()
        expect(pane.querySelector('.uam-composer-textarea')).toBeNull()
        expect(pane.querySelector('[aria-label="Send prompt"]')).toBeNull()
        expect(pane.querySelectorAll('nav[aria-label="Subagent lineage"]')).toHaveLength(1)
        const open = scopedAction(pane, 'Follow up')
        expect(open.getAttribute('aria-expanded')).toBe('false')
        open.focus()
        await act(async () => {
            open.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
            // jsdom does not synthesize native button keyboard default action; model that click explicitly.
            open.click()
        })
        const editor = pane.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
        expect(document.activeElement).toBe(editor)
        expect(pane.querySelector('[aria-label="Terminal fallback"], [aria-label="Chat view"], [aria-label^="Close "]')).toBeNull()
        act(() => enterNative(pane, 'Explicit actual native follow-up'))
        const back = scopedAction(pane, 'Back to parent'); back.focus()
        act(() => useAppStore.setState(state => ({ sessions: state.sessions.map(session => session.id === target.id ? { ...session, name: 'Renamed while reading' } : session) })))
        expect(pane.querySelector('.uam-composer-textarea')).toBe(editor)
        expect(editor.value).toBe('Explicit actual native follow-up')
        expect(document.activeElement).toBe(back)
        const send = useAppStore.getState().sendAcpPrompt
        await act(async () => pane.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })))
        expect(send).toHaveBeenCalledExactlyOnceWith(target.id, 'Explicit actual native follow-up', [], undefined, expect.any(Function))
    })

    it('actual Hide/reopen retains child text, attachments and quoted refs and Back restores independent parent context', async () => {
        const reference = captureContextReference({ id: 'native-access-reference', kind: 'message', sourceChatId: child.id,
            sourceMessageIndex: 0, sourceMessageId: `${child.id}-user`, executionHostId: 'local', workspaceDirectory: '',
            capturedAt: '2026-10-22T09:01:00Z', label: 'Own child context', text: 'Retain exactly this child quotation' })!
        const file = { id: 'native-access-file', name: 'child-owned.txt', type: 'file', mimeType: 'text/plain', size: 8, path: '/tmp/child-owned.txt' }
        writeChatComposerDraft(child.id, { text: 'Child original text', attachments: [file], references: [reference] })
        const { source, target } = await nativeProductRoute()
        const pane = actualNativePane()
        await act(async () => scopedAction(pane, 'Follow up').click())
        act(() => enterNative(pane, 'Child revised draft'))
        act(() => window.dispatchEvent(new Event('pagehide')))
        const saved = readChatComposerDraft(target.id)
        expect(saved.text).toBe('Child revised draft')
        expect(saved.attachments).toEqual([file]); expect(saved.references).toEqual([reference])
        const hide = scopedAction(pane, 'Hide follow-up input')
        await act(async () => hide.click())
        expect(document.activeElement).toBe(hide)
        expect(pane.querySelector('.uam-composer-textarea')).toBeNull()
        expect(readChatComposerDraft(target.id)).toEqual(saved)
        await act(async () => scopedAction(pane, 'Follow up').click())
        expect(pane.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe(saved.text)
        expect(pane.textContent).toContain('child-owned.txt')
        expect(readChatComposerDraft(target.id)).toEqual(saved)
        expect(useAppStore.getState().sendAcpPrompt).not.toHaveBeenCalled()
        expect(useAppStore.getState().stopAcpSession).not.toHaveBeenCalled()
        expect(useAppStore.getState().retryFailedMessage).not.toHaveBeenCalled()
        await act(async () => { scopedAction(pane, 'Back to parent').click(); flushFrame() })
        const returned = host.querySelector<HTMLElement>(`[data-testid="chat-pane-${source.id}"]`)!
        const parentInput = returned.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
        expect(parentInput.value).toBe('Independent parent native-access draft')
        expect(returned.querySelector<HTMLElement>('.uam-chat-transcript')!.scrollTop).toBe(220)
        expect(document.activeElement).toBe(parentInput)
        expect(readChatComposerDraft(target.id)).toEqual(saved)
    })

    it.each(['sibling', 'remount', 'new-birth'])('actual native follow-up visibility never carries across %s identity changes', async change => {
        writeChatComposerDraft(child.id, { text: 'A owned draft', attachments: [] })
        const { source, target } = await nativeProductRoute()
        await act(async () => scopedAction(actualNativePane(), 'Follow up').click())
        expect(actualNativePane().querySelector('.uam-composer-textarea')).not.toBeNull()
        if (change === 'sibling') {
            const sibling = { ...target, id: 'native-access-sibling', name: 'B child', createdAt: new Date(target.createdAt.getTime() + 1) }
            writeChatComposerDraft(sibling.id, { text: 'B owned draft', attachments: [] })
            act(() => useAppStore.setState(state => ({ sessions: [...state.sessions, sibling], messages: { ...state.messages, [sibling.id]: history(sibling) } })))
            await act(async () => openSubagentView(source, { kind: 'native', chatId: sibling.id }))
            expect(actualNativePane().querySelector('.uam-composer-textarea')).toBeNull()
            await act(async () => scopedAction(actualNativePane(), 'Follow up').click())
            expect(actualNativePane().querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe('B owned draft')
            await act(async () => openSubagentView(source, { kind: 'native', chatId: target.id }))
        } else if (change === 'remount') {
            await act(async () => root.render(<div />))
            await act(async () => root.render(<MainPanel />))
        } else {
            act(() => useAppStore.setState(state => ({ sessions: state.sessions.map(session => session.id === target.id
                ? { ...session, createdAt: new Date(session.createdAt.getTime() + 1) } : session) })))
        }
        expect(actualNativePane().querySelector('.uam-composer-textarea')).toBeNull()
        expect(scopedAction(actualNativePane(), 'Follow up').getAttribute('aria-expanded')).toBe('false')
        expect(useAppStore.getState().sendAcpPrompt).not.toHaveBeenCalled()
        expect(readChatComposerDraft(target.id).text).toBe('A owned draft')
    })

    it('binds native input visibility and transition focus to the exact pane when both panes show one child', async () => {
        const { source, target } = await nativeProductRoute()
        const layout = { version: 2 as const, activeLeafId: 'native-access-left', root: { type: 'split' as const,
            id: 'native-access-split', direction: 'horizontal' as const, sizes: [50, 50] as [number, number], children: [
                { type: 'leaf' as const, id: 'native-access-left', sessionId: source.id },
                { type: 'leaf' as const, id: 'native-access-right', sessionId: source.id },
            ] as [import('../../utils/chatGridStorage').ChatPaneLeaf, import('../../utils/chatGridStorage').ChatPaneLeaf] } }
        await act(async () => {
            writeChatGridLayout(layout)
            openSubagentView(source, { kind: 'native', chatId: target.id }, 'native-access-left')
            openSubagentView(source, { kind: 'native', chatId: target.id }, 'native-access-right')
            writeChatGridLayout({ ...readChatGridLayout(), activeLeafId: 'native-access-left' })
        })
        const panes = host.querySelectorAll<HTMLElement>(`[data-testid="subagent-pane-${source.id}"]`)
        expect(panes).toHaveLength(2)
        await act(async () => scopedAction(panes[0], 'Follow up').click())
        expect(panes[0].querySelector('.uam-composer-textarea')).not.toBeNull()
        expect(panes[1].querySelector('.uam-composer-textarea')).toBeNull()
        await act(async () => scopedAction(panes[1], 'Follow up').click())
        expect(document.activeElement).toBe(panes[1].querySelector('.uam-composer-textarea'))
        expect(scopedAction(panes[0], 'Hide follow-up input').getAttribute('aria-expanded')).toBe('true')
        expect(useAppStore.getState().sendAcpPrompt).not.toHaveBeenCalled()
    })

    it.each(['imported', 'foreign', 'unsupported', 'orphan'])('actual native access closes and is absent without valid executable lineage (%s)', async condition => {
        const { target } = await nativeProductRoute()
        await act(async () => scopedAction(actualNativePane(), 'Follow up').click())
        const refused = condition === 'imported' ? { ...target, importedReadOnly: true }
            : condition === 'unsupported' ? { ...target, providerId: 'claude-cli' }
            : { ...target, subagentParentChatId: condition === 'orphan' ? 'missing-parent' : 'foreign-parent' }
        act(() => useAppStore.setState(state => ({ sessions: state.sessions.map(session => session.id === target.id ? refused : session) })))
        if (condition === 'foreign' || condition === 'orphan') {
            // The old requested-parent route is gone; the selected owned Session now has its actual orphan route.
            expect(host.querySelector(`[data-testid="subagent-pane-${parent.id}"]`)).toBeNull()
            const own = host.querySelector<HTMLElement>(`[data-testid="orphan-subagent-pane-${target.id}"]`)
            expect(own).not.toBeNull()
            expect(scopedAction(own!, 'Follow up').getAttribute('aria-expanded')).toBe('false')
            expect(scopedAction(own!, 'Back to chats').disabled).toBe(false)
            expect(own!.querySelector('[aria-label="Close subagent view"]')).toBeNull()
        } else {
            expect(button('Follow up')).toBeUndefined()
        }
        expect(button('Hide follow-up input')).toBeUndefined()
        expect(host.querySelector('.uam-composer-textarea')).toBeNull()
        expect(useAppStore.getState().sendAcpPrompt).not.toHaveBeenCalled()
        expect(useAppStore.getState().startAcpSession).not.toHaveBeenCalled()
        expect(useAppStore.getState().stopAcpSession).not.toHaveBeenCalled()
        expect(useAppStore.getState().retryFailedMessage).not.toHaveBeenCalled()
        expect(useAppStore.getState().resumeQueuedAcpPrompts).not.toHaveBeenCalled()
    })

    it('keeps the actual sending then unknown child attempt through Hide/Open and remount without resubmission or retry', async () => {
        const { source, target } = await nativeProductRoute()
        let finish!: (accepted: boolean) => void
        const send = vi.fn((_id: string, _text: string, _attachments?: unknown[], _steer?: boolean, onStarted?: AcpPromptPresentationObserver) => {
            onStarted?.({ requestId: 'native-followup-access-origin', sessionId: target.id, createdAtMs: target.createdAt.getTime() })
            return new Promise<boolean>(resolve => { finish = resolve })
        })
        useAppStore.setState({ sendAcpPrompt: send })
        let pane = actualNativePane()
        await act(async () => scopedAction(pane, 'Follow up').click())
        act(() => enterNative(pane, 'Keep exact uncertain native input'))
        await act(async () => pane.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })))
        expect(send).toHaveBeenCalledExactlyOnceWith(target.id, 'Keep exact uncertain native input', [], undefined, expect.any(Function))
        const row = pane.querySelector('[data-outgoing-message]')!
        const article = row.querySelector('article')!
        expect(article.getAttribute('data-delivery')).toBe('sending')
        await act(async () => scopedAction(pane, 'Hide follow-up input').click())
        expect(pane.querySelector('[data-outgoing-message]')).toBe(row)
        await act(async () => scopedAction(pane, 'Follow up').click())
        expect(pane.querySelector('[data-outgoing-message]')).toBe(row)
        expect(send).toHaveBeenCalledTimes(1)
        await act(async () => finish(false))
        expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
        await act(async () => scopedAction(pane, 'Hide follow-up input').click())
        await act(async () => { scopedAction(pane, 'Back to parent').click(); flushFrame() })
        await act(async () => openSubagentView(source, { kind: 'native', chatId: target.id }))
        pane = actualNativePane()
        expect(pane.querySelector('.uam-composer-textarea')).toBeNull()
        expect(pane.querySelector('[data-outgoing-message] article')!.getAttribute('data-delivery')).toBe('unconfirmed')
        await act(async () => scopedAction(pane, 'Follow up').click())
        expect(pane.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe('Keep exact uncertain native input')
        expect(send).toHaveBeenCalledTimes(1)
        expect(useAppStore.getState().retryFailedMessage).not.toHaveBeenCalled()
        expect(useAppStore.getState().resumeQueuedAcpPrompts).not.toHaveBeenCalled()
        expect(useAppStore.getState().stopAcpSession).not.toHaveBeenCalled()
    })

    it('keeps native header refresh pending and errors with the displayed birth owner, never a prior deferred read', async () => {
        const { target } = await nativeProductRoute()
        const reads: Array<{ id: string; force?: boolean; resolve: (value: boolean) => void; reject: (error: Error) => void }> = []
        const load = vi.fn((id: string, force?: boolean) => {
            // Mount/update hydration reads complete normally; only explicit forced header reads are deferred.
            if (force !== true) return Promise.resolve(true)
            return new Promise<boolean>((resolve, reject) => { reads.push({ id, force, resolve, reject }) })
        })
        await act(async () => useAppStore.setState({ loadSessionMessages: load }))
        load.mockClear() // The new store action is committed before clicking the real header.
        const errorText = 'The conversation could not be refreshed. Loaded messages are still shown.'
        const refresh = () => scopedAction(actualNativePane(), 'Refresh conversation')
        const firstButton = refresh()
        await act(async () => firstButton.click())
        expect(load).toHaveBeenCalledExactlyOnceWith(target.id, true)
        expect(refresh().disabled).toBe(true)
        act(() => useAppStore.setState(state => ({ sessions: state.sessions.map(session => session.id === target.id
            ? { ...session, name: 'Unrelated rename keeps pending' } : session) })))
        expect(refresh().disabled).toBe(true)
        expect(reads).toHaveLength(1)

        act(() => useAppStore.setState(state => ({ sessions: state.sessions.map(session => session.id === target.id
            ? { ...session, createdAt: new Date(session.createdAt.getTime() + 1) } : session) })))
        // MainPanel's header remains mounted; its child ChatView may remount for the new birth.
        expect(refresh()).toBe(firstButton)
        expect(refresh().disabled).toBe(false)
        expect(actualNativePane().textContent).not.toContain(errorText)
        expect(scopedAction(actualNativePane(), 'Follow up').getAttribute('aria-expanded')).toBe('false')

        await act(async () => refresh().click())
        expect(reads).toHaveLength(2)
        expect(refresh().disabled).toBe(true)
        await act(async () => reads[0].reject(new Error('Prior birth A read rejected')))
        expect(refresh().disabled).toBe(true)
        expect(actualNativePane().textContent).not.toContain(errorText)
        await act(async () => reads[1].resolve(true))
        expect(refresh().disabled).toBe(false)
        expect(actualNativePane().textContent).not.toContain(errorText)

        await act(async () => refresh().click())
        expect(reads).toHaveLength(3)
        await act(async () => reads[2].resolve(false))
        expect(refresh().disabled).toBe(false)
        expect(actualNativePane().textContent).toContain(errorText)
        const dismiss = actualNativePane().querySelector<HTMLButtonElement>('[aria-label="Dismiss refresh error"]')
        expect(dismiss).not.toBeNull()
        await act(async () => dismiss!.click())
        expect(actualNativePane().textContent).not.toContain(errorText)
        await act(async () => refresh().click())
        expect(reads).toHaveLength(4)
        expect(refresh().disabled).toBe(true)
        expect(actualNativePane().textContent).not.toContain(errorText)
        await act(async () => reads[3].resolve(true))
        expect(refresh().disabled).toBe(false)
        expect(actualNativePane().textContent).not.toContain(errorText)
        expect(reads.map(({ id, force }) => [id, force])).toEqual(Array.from({ length: 4 }, () => [target.id, true]))
        expect(useAppStore.getState().sendAcpPrompt).not.toHaveBeenCalled()
        expect(useAppStore.getState().startAcpSession).not.toHaveBeenCalled()
        expect(useAppStore.getState().stopAcpSession).not.toHaveBeenCalled()
        expect(useAppStore.getState().retryFailedMessage).not.toHaveBeenCalled()
        expect(useAppStore.getState().resumeQueuedAcpPrompts).not.toHaveBeenCalled()
    })

    it('opens only the actual executable orphan Session input, keeps birth ownership and sends once with the five-argument observer', async () => {
        const own = { ...child, subagentParentChatId: 'unavailable-own-ancestor', subagentParentCreatedAt: parent.createdAt.toISOString() }
        writeChatComposerDraft(parent.id, { text: 'Unrelated parent resources stay here', attachments: [], references: [] })
        writeChatComposerDraft(own.id, { text: 'Own orphan draft', attachments: [], references: [] })
        await act(async () => {
            useAppStore.setState({ sessions: [parent, own], activeSessionId: own.id, acpBindingBySessionId: {},
                messages: { [parent.id]: history(parent), [own.id]: history(own) } })
            writeChatGridLayout(setChatInLeaf(defaultChatGridLayout, own.id, defaultChatGridLayout.activeLeafId))
            root.render(<MainPanel />)
        })
        const pane = () => host.querySelector<HTMLElement>(`[data-testid="orphan-subagent-pane-${own.id}"]`)!
        expect(pane()).not.toBeNull()
        expect(pane().querySelector('textarea')).toBeNull()
        expect(scopedAction(pane(), 'Back to chats').disabled).toBe(false)
        expect(pane().querySelector('[aria-label="Close subagent view"]')).toBeNull()
        expect(pane().textContent).toContain('Parent unavailable')
        await act(async () => scopedAction(pane(), 'Follow up').click())
        expect(pane().querySelector<HTMLTextAreaElement>('textarea')!.value).toBe('Own orphan draft')
        expect(document.activeElement).toBe(pane().querySelector('textarea'))
        act(() => enterNative(pane(), 'Own orphan revised draft'))
        await act(async () => scopedAction(pane(), 'Hide follow-up input').click())
        expect(pane().querySelector('textarea')).toBeNull()
        expect(useAppStore.getState().sendAcpPrompt).not.toHaveBeenCalled()
        await act(async () => scopedAction(pane(), 'Follow up').click())
        expect(pane().querySelector<HTMLTextAreaElement>('textarea')!.value).toBe('Own orphan revised draft')
        await act(async () => root.render(<div>Another pane is shown.</div>))
        await act(async () => root.render(<MainPanel />))
        expect(pane().querySelector('textarea')).toBeNull()
        await act(async () => scopedAction(pane(), 'Follow up').click())
        expect(pane().querySelector<HTMLTextAreaElement>('textarea')!.value).toBe('Own orphan revised draft')
        act(() => useAppStore.setState(state => ({ sessions: state.sessions.map(session => session.id === own.id
            ? { ...session, createdAt: new Date(session.createdAt.getTime() + 1) } : session) })))
        expect(pane().querySelector('textarea')).toBeNull()
        expect(scopedAction(pane(), 'Follow up').getAttribute('aria-expanded')).toBe('false')
        await act(async () => scopedAction(pane(), 'Follow up').click())
        act(() => enterNative(pane(), 'Explicit current orphan send'))
        await act(async () => pane().querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })))
        expect(useAppStore.getState().sendAcpPrompt).toHaveBeenCalledExactlyOnceWith(own.id, 'Explicit current orphan send', [], undefined, expect.any(Function))
        expect(readChatComposerDraft(parent.id).text).toBe('Unrelated parent resources stay here')
        expect(useAppStore.getState().sessions.some(session => session.id === 'unavailable-own-ancestor')).toBe(false)
        expect(useAppStore.getState().sessions.find(session => session.id === own.id)?.subagentParentChatId).toBe('unavailable-own-ancestor')
    })

    it.each(['imported', 'unsupported', 'absent'])('never exposes orphan input without the actual current executable Session (%s)', async condition => {
        const own = { ...child, importedReadOnly: condition === 'imported', providerId: condition === 'unsupported' ? 'claude-cli' : child.providerId,
            subagentParentChatId: 'unavailable-own-ancestor', subagentParentCreatedAt: parent.createdAt.toISOString() }
        await act(async () => {
            useAppStore.setState({ sessions: condition === 'absent' ? [] : [own], activeSessionId: own.id, acpBindingBySessionId: {},
                messages: { [own.id]: history(own) } })
            writeChatGridLayout(setChatInLeaf(defaultChatGridLayout, own.id, defaultChatGridLayout.activeLeafId))
            root.render(<MainPanel />)
        })
        expect(button('Follow up')).toBeUndefined()
        expect(button('Hide follow-up input')).toBeUndefined()
        expect(host.querySelector('textarea')).toBeNull()
        expect(useAppStore.getState().sendAcpPrompt).not.toHaveBeenCalled()
        expect(useAppStore.getState().startAcpSession).not.toHaveBeenCalled()
        expect(useAppStore.getState().sessions.some(session => session.id === 'unavailable-own-ancestor')).toBe(false)
    })

    it.each(['foreign', 'orphan'])('reconciles an invalidated parent request into a closed own orphan route without borrowing input (%s)', async condition => {
        const { source, target } = await nativeProductRoute()
        const requestedPane = actualNativePane()
        await act(async () => scopedAction(requestedPane, 'Follow up').click())
        const oldField = requestedPane.querySelector<HTMLTextAreaElement>('textarea')!
        expect(oldField).not.toBeNull()
        const parentDraft = readChatComposerDraft(source.id)
        const nextParentId = condition === 'orphan' ? 'missing-parent' : 'foreign-parent'
        act(() => useAppStore.setState(state => ({ sessions: state.sessions.map(session => session.id === target.id
            ? { ...session, subagentParentChatId: nextParentId } : session) })))
        expect(host.querySelector(`[data-testid="subagent-pane-${source.id}"]`)).toBeNull()
        expect(requestedPane.isConnected).toBe(false)
        expect(oldField.isConnected).toBe(false)
        const current = host.querySelector<HTMLElement>(`[data-testid="orphan-subagent-pane-${target.id}"]`)
        expect(current).not.toBeNull()
        expect(current!.querySelector(`[data-testid="chat-pane-${target.id}"]`)).not.toBeNull()
        expect(current!.querySelector('textarea')).toBeNull()
        expect(scopedAction(current!, 'Follow up').getAttribute('aria-expanded')).toBe('false')
        expect(scopedAction(current!, 'Back to chats').disabled).toBe(false)
        expect(current!.querySelector('[aria-label="Close subagent view"]')).toBeNull()
        expect(useAppStore.getState().sessions.find(session => session.id === target.id)?.createdAt).toBe(target.createdAt)
        expect(useAppStore.getState().sessions.find(session => session.id === target.id)?.subagentParentChatId).toBe(nextParentId)
        expect(useAppStore.getState().sessions.some(session => session.id === nextParentId)).toBe(false)
        expect(readChatComposerDraft(source.id)).toEqual(parentDraft)
        expect(useAppStore.getState().sendAcpPrompt).not.toHaveBeenCalled()
        expect(useAppStore.getState().startAcpSession).not.toHaveBeenCalled()
        expect(useAppStore.getState().stopAcpSession).not.toHaveBeenCalled()
        expect(useAppStore.getState().retryFailedMessage).not.toHaveBeenCalled()
        expect(useAppStore.getState().resumeQueuedAcpPrompts).not.toHaveBeenCalled()
    })
})


// Separate later full-history requirement; reviewed cb5d and the executable-child ca47 control stay independent.
describe('passive child rail through MainPanel and real store', () => {
    it('requests one bounded passive-child target page and keeps the parent runtime and reading pane', async () => {
        const fixtureState = useAppStore.getState()
        const fixtureCef = window.cefQuery
        const fixtureLayout = readChatGridLayout()
        const source = parent
        const target: Session = { ...child, id: 'composer-contract-passive-target-child', importedReadOnly: true,
            createdAt: new Date('2026-10-22T10:05:00Z'), updatedAt: new Date('2026-10-22T10:05:00Z'),
            subagentParentChatId: source.id, subagentParentCreatedAt: source.createdAt.toISOString(),
            messagesDigest: 'passive-rail-1200-catalogue-v1', messageCount: 1200,
            userMessageIndices: Array.from({ length: 600 }, (_, index) => index * 2) }
        const realOlder = previous.loadOlderSessionMessages
        const realSelection = previous.setActiveSession
        const realInitialRead = previous.loadSessionMessages
        expect(fixtureState.loadOlderSessionMessages).toBe(realOlder)
        expect(fixtureState.setActiveSession).toBe(realSelection)
        expect(vi.isMockFunction(realOlder)).toBe(false)
        expect(vi.isMockFunction(realSelection)).toBe(false)
        expect(vi.isMockFunction(realInitialRead)).toBe(false)
        expect(Number.isFinite(target.createdAt.getTime())).toBe(true)
        expect(target.importedReadOnly).toBe(true)
        expect(canExecuteNativeSubagent(target)).toBe(false)
        const book: Message[] = Array.from({ length: 1200 }, (_, index) => ({ id: `passive-target-${index}`,
            sessionId: target.id, role: index % 2 ? 'assistant' : 'user', content: `Passive child exact row ${index}`,
            createdAt: new Date(target.createdAt.getTime() + index * 1000) }))
        const wire = (start: number, end: number) => book.slice(start, end).map(message => ({
            role: message.role, content: message.content, createdAt: message.createdAt.toISOString(),
        }))
        const pages = Array.from({ length: 12 }, (_, index) => ({ before: Math.max(0, 1150 - index * 100), start: Math.max(0, 1150 - (index + 1) * 100) }))
        const reference = captureContextReference({ id: 'passive-rail-parent-context', kind: 'message', sourceChatId: source.id,
            sourceMessageIndex: 0, sourceMessageId: `${source.id}-user`, executionHostId: 'local', workspaceDirectory: '',
            capturedAt: '2026-10-22T10:06:00Z', label: 'Parent-only resources', text: 'Never borrow parent quoted content' })!
        expect(reference).not.toBeNull()
        writeChatComposerDraft(source.id, { text: 'Parent draft while passive rail reads', attachments: [
            { id: 'passive-rail-parent-file', name: 'parent-only.txt', type: 'file', mimeType: 'text/plain', size: 4, path: '/tmp/parent-only.txt' },
        ], references: [reference] })
        type Envelope = { action: string; requestId?: string; payload?: Record<string, unknown> }
        const envelopes: Envelope[] = []
        const targetRequests: { envelope: Envelope; settled: boolean; complete: () => void }[] = []
        const olderReplies: { before: unknown; start: number | null; count: number; ok: boolean }[] = []
        let successfulOlder = 0
        const projection: { active: string | null; search: unknown }[] = []
        let unsubscribe = () => {}
        window.cefQuery = ({ request, onSuccess }) => {
            const envelope = JSON.parse(request) as Envelope
            envelopes.push(envelope)
            const payload = envelope.payload
            if (envelope.action === 'getChatMessages' && payload?.targetMessageIndex !== undefined) {
                const pending = { envelope, settled: false, complete: () => {
                    if (pending.settled) return
                    pending.settled = true
                    onSuccess(JSON.stringify(payload.chatId === target.id
                        ? { ok: true, data: { chatId: target.id, messagesDigest: target.messagesDigest, startIndex: 0,
                            totalCount: 1200, userMessageIndices: target.userMessageIndices, messages: wire(0, 26) } }
                        : { ok: false, error: 'Fixture refuses a target read outside its own passive child.' }))
                } }
                targetRequests.push(pending)
            } else if (envelope.action === 'getChatMessages' && payload?.before !== undefined) {
                const page = pages[successfulOlder]
                // At most twelve contiguous, <=100-row pages; malformed/extra reads receive an explicit finite failure.
                if (payload.chatId !== target.id || payload.limit !== 100 || !page || payload.before !== page.before) {
                    olderReplies.push({ before: payload.before, start: null, count: 0, ok: false })
                    onSuccess(JSON.stringify({ ok: false, error: 'Fixture older-page budget or contiguous child boundary exhausted.' }))
                } else {
                    successfulOlder += 1
                    olderReplies.push({ before: page.before, start: page.start, count: page.before - page.start, ok: true })
                    onSuccess(JSON.stringify({ ok: true, data: { chatId: target.id, messagesDigest: target.messagesDigest,
                        startIndex: page.start, totalCount: 1200, userMessageIndices: target.userMessageIndices,
                        messages: wire(page.start, page.before) } }))
                }
            } else if (envelope.action === 'getChatMessages' && payload?.chatId === target.id) {
                // Real initial hydration primes loadedMessagesDigestByChatId before real older-page reads can succeed.
                onSuccess(JSON.stringify({ ok: true, data: { chatId: target.id, messagesDigest: target.messagesDigest,
                    startIndex: 1150, totalCount: 1200, userMessageIndices: target.userMessageIndices, messages: wire(1150, 1200) } }))
            } else if (envelope.action === 'getChatMessages') {
                onSuccess(JSON.stringify({ ok: true, data: { chatId: source.id, messagesDigest: source.messagesDigest,
                    unchanged: true, totalCount: source.messageCount, userMessageIndices: source.userMessageIndices } }))
            } else onSuccess(JSON.stringify({ ok: true, data: {} }))
        }
        const positions = new WeakMap<Element, number>()
        const passiveRegion = (element: Element) => element.classList.contains('uam-subagent-transcript')
        const rendered = (element: Element) => [...element.querySelectorAll<HTMLElement>('[data-message-index]')]
        const height = (element: Element) => 40 + ((element.textContent?.length ?? 0) % 3) * 4
        const offset = (row: Element, owner: Element) => {
            let top = 0
            for (const candidate of rendered(owner)) { if (candidate === row) return top; top += height(candidate) }
            throw new Error('alignment addressed a row outside the actual passive transcript')
        }
        const rect = (top: number, size: number) => ({ x: 0, y: top, top, bottom: top + size, left: 0, right: 400, width: 400, height: size, toJSON: () => ({ top, height: size }) }) as DOMRect
        const geometrySpies = [
            vi.spyOn(Element.prototype, 'scrollHeight', 'get').mockImplementation(function (this: Element) { return passiveRegion(this) ? rendered(this).reduce((sum, row) => sum + height(row), 0) : 0 }),
            vi.spyOn(Element.prototype, 'clientHeight', 'get').mockImplementation(function (this: Element) { return passiveRegion(this) ? 280 : 0 }),
            vi.spyOn(Element.prototype, 'scrollTop', 'get').mockImplementation(function (this: Element) { return positions.get(this) ?? 0 }),
            vi.spyOn(Element.prototype, 'scrollTop', 'set').mockImplementation(function (this: Element, top: number) { positions.set(this, Math.max(0, Math.min(top, Math.max(0, this.scrollHeight - this.clientHeight)))) }),
            vi.spyOn(Element.prototype, 'getBoundingClientRect').mockImplementation(function (this: Element) {
                if (passiveRegion(this)) return rect(100, 280)
                if (this.getAttribute('role') === 'slider') return rect(20, 200)
                const owner = this.closest('.uam-subagent-transcript')
                return owner && this.hasAttribute('data-message-index') ? rect(100 + offset(this, owner) - owner.scrollTop, height(this)) : rect(0, 0)
            }),
        ]
        const previousAlignment = Object.getOwnPropertyDescriptor(Element.prototype, 'scrollIntoView')
        const aligned: { index: number; block: string | undefined }[] = []
        Object.defineProperty(Element.prototype, 'scrollIntoView', { configurable: true, value: function (this: Element, options?: boolean | ScrollIntoViewOptions) {
            const owner = this.closest('.uam-subagent-transcript')
            if (!owner || !this.hasAttribute('data-message-index')) return
            owner.scrollTop = offset(this, owner)
            aligned.push({ index: Number(this.getAttribute('data-message-index')), block: typeof options === 'object' ? options.block : undefined })
        } })
        try {
            rememberSubagentParent(source, target.id)
            await act(async () => {
                useAppStore.setState({ sessions: [source, target], activeSessionId: source.id,
                    messages: { [source.id]: history(source), [target.id]: book.slice(1150) }, acpBindingBySessionId: {},
                    historyStartIndexBySessionId: { [source.id]: 0, [target.id]: 1150 }, chatHistoryErrorBySessionId: {}, chatSearchNavigation: null,
                    loadSessionMessages: realInitialRead,
                    stopAcpSession: vi.fn().mockResolvedValue(true), retryFailedMessage: vi.fn().mockResolvedValue({ ok: false }),
                    resumeQueuedAcpPrompts: vi.fn().mockResolvedValue(false) })
                writeChatGridLayout(setChatInLeaf(defaultChatGridLayout, source.id, defaultChatGridLayout.activeLeafId))
                root.render(<MainPanel />)
            })
            const parentPane = host.querySelector<HTMLElement>(`[data-testid="chat-pane-${source.id}"]`)!
            expect(parentPane).not.toBeNull()
            const parentScroll = parentPane.querySelector<HTMLElement>('.uam-chat-transcript')!
            Object.defineProperties(parentScroll, { scrollHeight: { configurable: true, value: 2000 }, clientHeight: { configurable: true, value: 300 } })
            parentScroll.scrollTop = 220
            await act(async () => { parentScroll.dispatchEvent(new Event('scroll', { bubbles: true })); flushFrame() })
            const parentDraft = readChatComposerDraft(source.id)
            const parentMessages = useAppStore.getState().messages[source.id]
            const leafId = readChatGridLayout().activeLeafId
            // The real passive selection path opens the child route while retaining parent runtime selection.
            await act(async () => useAppStore.getState().setActiveSession(target.id, { leafId }))
            await act(async () => { flushFrame() })
            expect(useAppStore.getState().activeSessionId).toBe(source.id)
            expect(useAppStore.getState().setActiveSession).toBe(realSelection)
            expect(useAppStore.getState().loadOlderSessionMessages).toBe(realOlder)
            expect(useAppStore.getState().loadSessionMessages).toBe(realInitialRead)
            expect(useAppStore.getState().chatSearchNavigation).toBeNull()
            const current = useAppStore.getState().sessions.find(item => item.id === target.id)!
            expect(current).toBeDefined()
            expect(current.createdAt.getTime()).toBe(target.createdAt.getTime())
            expect(current.messagesDigest).toBe(target.messagesDigest)
            expect(current.messageCount).toBe(1200)
            expect(current.userMessageIndices).toEqual(target.userMessageIndices)
            expect(current.importedReadOnly).toBe(true)
            const pane = host.querySelector<HTMLElement>(`[data-testid="subagent-pane-${source.id}"]`)!
            expect(pane).not.toBeNull()
            const native = pane.querySelector<HTMLElement>('section[aria-label="Native subagent conversation"]')!
            expect(native).not.toBeNull()
            const viewport = native.querySelector<HTMLElement>('[role="region"][aria-label="Native conversation messages"]')!
            expect(viewport).not.toBeNull()
            expect(viewport.querySelector('.uam-chat-content .uam-message-list [data-message-index="1198"]')?.textContent).toContain('Passive child exact row 1198')
            expect(viewport.querySelector('[data-message-index="0"]')).toBeNull()
            expect(viewport.textContent).not.toContain('Parent task answer')
            expect(viewport.scrollHeight).toBeGreaterThan(viewport.clientHeight + 320)
            expect(useAppStore.getState().historyStartIndexBySessionId[target.id]).toBe(1150)
            expect(useAppStore.getState().messages[target.id]).toHaveLength(50)
            expect(envelopes.some(item => item.action === 'getChatMessages' && item.payload?.chatId === target.id && item.payload?.limit === 50 && item.payload?.before === undefined && item.payload?.targetMessageIndex === undefined)).toBe(true)
            expect(olderReplies, 'tall initial content must not auto-fill history before rail intent').toEqual([])
            expect(targetRequests).toEqual([])
            const rail = native.querySelector<HTMLElement>('[role="slider"][aria-label="Navigate user messages"]')!
            expect(rail).not.toBeNull()
            expect(rail.getAttribute('aria-valuemax')).toBe('600')
            expect(rail.getAttribute('aria-disabled')).toBe('false')
            expect(native.querySelector('textarea, input, form')).toBeNull()
            const route = subagentRouteFor(leafId, source)
            expect(route).toMatchObject({ kind: 'native', chatId: target.id, parentId: source.id })
            unsubscribe = useAppStore.subscribe(state => projection.push({ active: state.activeSessionId, search: state.chatSearchNavigation }))
            const railBoundary = envelopes.length
            await act(async () => {
                rail.dispatchEvent(new MouseEvent('mousedown', { bubbles: true, clientY: 20 }))
                rail.focus()
                rail.dispatchEvent(new MouseEvent('mouseup', { bubbles: true, clientY: 20 }))
                rail.dispatchEvent(new MouseEvent('click', { bubbles: true, clientY: 20 }))
            })
            const boundary = JSON.stringify({ targetRequests: targetRequests.length, successfulOlder, olderReplies,
                remainingBefore: useAppStore.getState().historyStartIndexBySessionId[target.id],
                sharedSearch: useAppStore.getState().chatSearchNavigation, runtime: useAppStore.getState().activeSessionId })
            // Intended missing-producer oracle comes before target DOM/geometry; there is no minimum sequential-read prerequisite.
            expect(targetRequests, `passive rail must produce exactly one bounded child target read; ${boundary}`).toHaveLength(1)
            expect(targetRequests[0].envelope).toMatchObject({ action: 'getChatMessages', requestId: expect.any(String), payload: {
                chatId: target.id, limit: 50, before: 26, deferToolCallContent: true,
                expectedMessagesDigest: target.messagesDigest, targetMessageIndex: 0,
            } })
            expect(targetRequests[0].envelope.payload).not.toHaveProperty('refreshNative')
            await act(async () => targetRequests[0].complete())
            expect(targetRequests.every(item => item.settled)).toBe(true)
            const row = viewport.querySelector<HTMLElement>('[data-message-index="0"]')
            expect(row, 'exact early row must be adopted by the actual passive route').not.toBeNull()
            expect(row!.textContent).toContain('Passive child exact row 0')
            expect(aligned).toContainEqual({ index: 0, block: 'start' })
            expect(row!.getBoundingClientRect().top).toBe(viewport.getBoundingClientRect().top)
            expect(row!.getBoundingClientRect().bottom).toBeLessThanOrEqual(viewport.getBoundingClientRect().bottom)
            expect(host.querySelector(`[data-testid="subagent-pane-${source.id}"]`)).toBe(pane)
            expect(pane.querySelector('section[aria-label="Native subagent conversation"]')).toBe(native)
            expect(useAppStore.getState().activeSessionId).toBe(source.id)
            expect(useAppStore.getState().chatSearchNavigation).toBeNull()
            expect(projection.every(item => item.active === source.id && item.search === null)).toBe(true)
            expect(subagentRouteFor(leafId, source)).toEqual(route)
            expect(readChatGridLayout().activeLeafId).toBe(leafId)
            expect(chatGridLeaves(readChatGridLayout().root).find(item => item.id === leafId)?.sessionId).toBe(source.id)
            expect(readChatComposerDraft(source.id)).toEqual(parentDraft)
            expect(parentDraft.text).toBe('Parent draft while passive rail reads')
            expect(parentDraft.attachments).toHaveLength(1)
            expect(parentDraft.references).toEqual([reference])
            expect(useAppStore.getState().messages[source.id]).toBe(parentMessages)
            expect(envelopes.slice(railBoundary).filter(item => item.action === 'selectSession' || item.payload?.refreshNative === true)).toEqual([])
            expect(envelopes.slice(railBoundary).filter(item => item.action === 'getChatMessages' && item.payload?.chatId !== target.id)).toEqual([])
            expect(native.querySelector('textarea, input, form')).toBeNull()
            expect([...native.querySelectorAll('button')].some(item => /^(Follow up|Stop|Retry message|Resume)/i.test(item.getAttribute('aria-label') || item.textContent?.trim() || ''))).toBe(false)
            const state = useAppStore.getState()
            expect(state.sendAcpPrompt).not.toHaveBeenCalled()
            expect(state.startAcpSession).not.toHaveBeenCalled()
            expect(state.stopAcpSession).not.toHaveBeenCalled()
            expect(state.retryFailedMessage).not.toHaveBeenCalled()
            expect(state.resumeQueuedAcpPrompts).not.toHaveBeenCalled()
            expect(successfulOlder).toBeLessThanOrEqual(12)
        } finally {
            unsubscribe()
            await act(async () => { root.render(<div />); for (const pending of targetRequests) pending.complete() })
            // Clear only this unique fixture child's real history cache while all native transport is still fake.
            await act(async () => previous.unloadSessionMessages(target.id))
            for (const leaf of chatGridLeaves(readChatGridLayout().root)) clearSubagentView(leaf.id)
            writeChatGridLayout(fixtureLayout)
            removeComposerDrafts([source.id, target.id])
            window.cefQuery = fixtureCef
            useAppStore.setState(fixtureState, true)
            frames.clear()
            if (previousAlignment) Object.defineProperty(Element.prototype, 'scrollIntoView', previousAlignment)
            else Reflect.deleteProperty(Element.prototype, 'scrollIntoView')
            for (const spy of geometrySpies) spy.mockRestore()
            expect(targetRequests.every(item => item.settled)).toBe(true)
            // Existing file afterEach restores original store/CEF/DOM/browser globals; no held callback survives a failed oracle.
        }
    })
})


// Separate later regression: executable runtime selection is the child; parent leaf/return context stays fixed.
describe('executable child rail with real store and fake CEF', () => {
    it('loads the child rail target through real store actions and retains the parent pane and Back context', async () => {
        const fixtureState = useAppStore.getState()
        const fixtureCef = window.cefQuery
        const fixtureLayout = readChatGridLayout()
        const source = parent
        const target: Session = { ...child, id: 'composer-contract-real-rail-child', importedReadOnly: false,
            createdAt: new Date('2026-10-22T09:03:00Z'), updatedAt: new Date('2026-10-22T09:03:00Z'),
            subagentParentChatId: source.id, subagentParentCreatedAt: source.createdAt.toISOString(),
            messagesDigest: 'real-rail-child-catalogue-v1', messageCount: 100, userMessageIndices: [0, 50, 98] }
        const realJump = previous.jumpToSessionMessage
        const realSearchJump = previous.jumpToChatSearchHit
        const realSelection = previous.setActiveSession
        expect(useAppStore.getState().jumpToSessionMessage).toBe(realJump)
        expect(useAppStore.getState().jumpToChatSearchHit).toBe(realSearchJump)
        expect(useAppStore.getState().setActiveSession).toBe(realSelection)
        expect(vi.isMockFunction(realJump)).toBe(false)
        expect(vi.isMockFunction(realSearchJump)).toBe(false)
        expect(vi.isMockFunction(realSelection)).toBe(false)
        expect(Number.isFinite(target.createdAt.getTime())).toBe(true)
        expect(canExecuteNativeSubagent(target)).toBe(true)
        expect(target.messagesDigest).toBeTruthy()
        expect(target.messageCount).toBe(100)
        expect(target.userMessageIndices).toEqual([0, 50, 98])
        const book: Message[] = Array.from({ length: 100 }, (_, index) => ({ id: `real-rail-${index}`,
            sessionId: target.id, role: index % 2 ? 'assistant' : 'user', content: `Actual child rail row ${index}`,
            createdAt: new Date(target.createdAt.getTime() + index * 1000) }))
        const reference = captureContextReference({ id: 'real-rail-parent-reference', kind: 'message', sourceChatId: source.id,
            sourceMessageIndex: 0, sourceMessageId: `${source.id}-user`, executionHostId: 'local', workspaceDirectory: '',
            capturedAt: '2026-10-22T09:04:00Z', label: 'Parent-only context', text: 'Keep exact parent quoted resources' })!
        expect(reference).not.toBeNull()
        writeChatComposerDraft(source.id, { text: 'Parent draft before real child rail', attachments: [
            { id: 'rail-parent-file', name: 'parent-owned.txt', type: 'file', mimeType: 'text/plain', size: 4, path: '/tmp/parent-owned.txt' },
        ], references: [reference] })
        writeChatComposerDraft(target.id, { text: 'Independent hidden child rail draft', attachments: [], references: [] })
        type Envelope = { action: string; requestId?: string; payload?: Record<string, unknown> }
        const envelopes: Envelope[] = []
        const held: { envelope: Envelope; settled: boolean; complete: () => void }[] = []
        // Only target-addressed history is held. Real selection and its ordinary hydration finish normally.
        window.cefQuery = ({ request, onSuccess }) => {
            const envelope = JSON.parse(request) as Envelope
            envelopes.push(envelope)
            if (envelope.action === 'getChatMessages' && envelope.payload?.targetMessageIndex !== undefined) {
                const pending = { envelope, settled: false, complete: () => {
                    if (pending.settled) return
                    pending.settled = true
                    onSuccess(JSON.stringify({ ok: true, data: { chatId: target.id, messagesDigest: target.messagesDigest,
                        startIndex: 0, totalCount: 100, userMessageIndices: target.userMessageIndices,
                        messages: book.slice(0, 26).map(message => ({ role: message.role, content: message.content,
                            createdAt: message.createdAt.toISOString() })) } }))
                } }
                held.push(pending)
            } else if (envelope.action === 'getChatMessages') {
                const own = envelope.payload?.chatId === target.id ? target : source
                onSuccess(JSON.stringify({ ok: true, data: { chatId: own.id, messagesDigest: own.messagesDigest,
                    unchanged: true, totalCount: own.messageCount, userMessageIndices: own.userMessageIndices } }))
            } else onSuccess(JSON.stringify({ ok: true, data: {} }))
        }
        const expectNoInputActions = () => {
            const state = useAppStore.getState()
            expect(state.sendAcpPrompt).not.toHaveBeenCalled()
            expect(state.startAcpSession).not.toHaveBeenCalled()
            expect(state.stopAcpSession).not.toHaveBeenCalled()
            expect(state.retryFailedMessage).not.toHaveBeenCalled()
            expect(state.resumeQueuedAcpPrompts).not.toHaveBeenCalled()
            expect(envelopes.filter(item => /sendAcp|startAcp|stopAcp|retry.*Message|resumeQueued/i.test(item.action))).toEqual([])
        }
        try {
            rememberSubagentParent(source, target.id)
            await act(async () => {
                useAppStore.setState({ sessions: [source, target], activeSessionId: source.id,
                    messages: { [source.id]: history(source), [target.id]: book.slice(50) }, acpBindingBySessionId: {},
                    historyStartIndexBySessionId: { [source.id]: 0, [target.id]: 50 }, chatSearchNavigation: null,
                    stopAcpSession: vi.fn().mockResolvedValue(true), retryFailedMessage: vi.fn().mockResolvedValue({ ok: false }),
                    resumeQueuedAcpPrompts: vi.fn().mockResolvedValue(false) })
                writeChatGridLayout(setChatInLeaf(defaultChatGridLayout, source.id, defaultChatGridLayout.activeLeafId))
                root.render(<MainPanel />)
            })
            const parentPane = host.querySelector<HTMLElement>(`[data-testid="chat-pane-${source.id}"]`)!
            expect(parentPane).not.toBeNull()
            const parentScroll = parentPane.querySelector<HTMLElement>('.uam-chat-transcript')!
            Object.defineProperties(parentScroll, { scrollHeight: { configurable: true, value: 2000 }, clientHeight: { configurable: true, value: 300 } })
            parentScroll.scrollTop = 220
            await act(async () => { parentScroll.dispatchEvent(new Event('scroll', { bubbles: true })); flushFrame() })
            const parentDraft = readChatComposerDraft(source.id)
            expect(parentDraft.text).toBe('Parent draft before real child rail')
            expect(parentDraft.attachments).toHaveLength(1)
            expect(parentDraft.references).toEqual([reference])
            const parentMessages = useAppStore.getState().messages[source.id]
            const leafId = readChatGridLayout().activeLeafId
            await act(async () => openSubagentView(source, { kind: 'native', chatId: target.id }, leafId))
            // Settle the real MainPanel activation, including selectSession's fake CEF receipt, before rail intent.
            await act(async () => { flushFrame() })
            expect(useAppStore.getState().activeSessionId).toBe(target.id)
            expect(envelopes.some(item => item.action === 'selectSession' && item.payload?.chatId === target.id)).toBe(true)
            expect(useAppStore.getState().jumpToSessionMessage).toBe(realJump)
            expect(useAppStore.getState().jumpToChatSearchHit).toBe(realSearchJump)
            expect(useAppStore.getState().setActiveSession).toBe(realSelection)
            const liveChild = useAppStore.getState().sessions.find(item => item.id === target.id)!
            expect(liveChild).toBeDefined()
            expect(liveChild.createdAt.getTime()).toBe(target.createdAt.getTime())
            expect(canExecuteNativeSubagent(liveChild)).toBe(true)
            expect(liveChild.messagesDigest).toBe(target.messagesDigest)
            expect(liveChild.messageCount).toBe(100)
            expect(liveChild.userMessageIndices).toEqual([0, 50, 98])
            const pane = host.querySelector<HTMLElement>(`[data-testid="subagent-pane-${source.id}"]`)!
            expect(pane).not.toBeNull()
            const ownPane = pane.querySelector<HTMLElement>(`[data-testid="chat-pane-${target.id}"]`)!
            expect(ownPane).not.toBeNull()
            expect(ownPane.querySelector('.uam-composer-textarea')).toBeNull()
            expect(ownPane.querySelector('[data-message-index="0"]')).toBeNull()
            expect(useAppStore.getState().historyStartIndexBySessionId[target.id]).toBe(50)
            const route = subagentRouteFor(leafId, source)
            expect(route).toMatchObject({ kind: 'native', chatId: target.id, parentId: source.id })
            expect(chatGridLeaves(readChatGridLayout().root).find(item => item.id === leafId)?.sessionId).toBe(source.id)
            const rail = ownPane.querySelector<HTMLElement>('[role="slider"][aria-label="Navigate user messages"]')!
            expect(rail).not.toBeNull()
            expect(rail.getAttribute('aria-valuemax')).toBe('3')
            expect(rail.getAttribute('aria-disabled')).toBe('false')
            vi.spyOn(rail, 'getBoundingClientRect').mockReturnValue({ top: 20, height: 200, bottom: 220, left: 0, right: 20, width: 20, x: 0, y: 20, toJSON: () => ({}) } as DOMRect)
            const railBoundary = envelopes.length
            await act(async () => {
                rail.dispatchEvent(new MouseEvent('mousedown', { bubbles: true, clientY: 20 }))
                rail.focus()
                rail.dispatchEvent(new MouseEvent('mouseup', { bubbles: true, clientY: 20 }))
                rail.dispatchEvent(new MouseEvent('click', { bubbles: true, clientY: 20 }))
            })
            // Positive request prerequisite precedes every pane/parent preservation oracle.
            expect(held).toHaveLength(1)
            expect(held[0].envelope).toMatchObject({ action: 'getChatMessages', requestId: expect.any(String), payload: {
                chatId: target.id, limit: 50, before: 26, deferToolCallContent: true,
                expectedMessagesDigest: target.messagesDigest, targetMessageIndex: 0,
            } })
            await act(async () => { held[0].complete() })
            expect(held.every(item => item.settled)).toBe(true)
            expect(useAppStore.getState().historyStartIndexBySessionId[target.id]).toBe(0)
            expect(useAppStore.getState().messages[target.id]?.[0].content).toBe('Actual child rail row 0')
            expect(useAppStore.getState().activeSessionId).toBe(target.id)
            expect(host.querySelector(`[data-testid="subagent-pane-${source.id}"]`)).toBe(pane)
            expect(pane.querySelector(`[data-testid="chat-pane-${target.id}"]`)).toBe(ownPane)
            expect(subagentRouteFor(leafId, source)).toEqual(route)
            expect(readChatGridLayout().activeLeafId).toBe(leafId)
            expect(chatGridLeaves(readChatGridLayout().root).find(item => item.id === leafId)?.sessionId).toBe(source.id)
            expect(ownPane.querySelector('[data-message-index="0"]')?.textContent).toContain('Actual child rail row 0')
            expect(ownPane.querySelector('.uam-composer-textarea')).toBeNull()
            expect(readChatComposerDraft(source.id)).toEqual(parentDraft)
            expect(useAppStore.getState().messages[source.id]).toBe(parentMessages)
            expect(envelopes.slice(railBoundary).filter(item => item.action === 'getChatMessages' && item.payload?.chatId === source.id)).toEqual([])
            expectNoInputActions()
            const back = [...pane.querySelectorAll<HTMLButtonElement>('nav button')].find(item => (item.getAttribute('aria-label') || item.textContent?.trim()) === 'Back to parent')!
            expect(back).toBeDefined()
            await act(async () => { back.click(); flushFrame() })
            const returned = host.querySelector<HTMLElement>(`[data-testid="chat-pane-${source.id}"]`)!
            expect(returned).not.toBeNull()
            expect(useAppStore.getState().activeSessionId).toBe(source.id)
            expect(subagentRouteFor(leafId, source)).toBeUndefined()
            expect(readChatGridLayout().activeLeafId).toBe(leafId)
            expect(chatGridLeaves(readChatGridLayout().root).find(item => item.id === leafId)?.sessionId).toBe(source.id)
            expect(returned.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')?.value).toBe(parentDraft.text)
            expect(returned.querySelector<HTMLElement>('.uam-chat-transcript')?.scrollTop).toBe(220)
            expect(readChatComposerDraft(source.id)).toEqual(parentDraft)
            expect(useAppStore.getState().messages[source.id]).toBe(parentMessages)
            expect(readChatComposerDraft(target.id).text).toBe('Independent hidden child rail draft')
            expectNoInputActions()
        } finally {
            // Unmount first, then complete every held native-read envelope even when an oracle throws.
            await act(async () => { root.render(<div />); for (const pending of held) pending.complete() })
            expect(held.every(item => item.settled)).toBe(true)
            for (const leaf of chatGridLeaves(readChatGridLayout().root)) clearSubagentView(leaf.id)
            writeChatGridLayout(fixtureLayout)
            removeComposerDrafts([source.id, target.id])
            window.cefQuery = fixtureCef
            useAppStore.setState(fixtureState, true)
            frames.clear()
            // File afterEach restores the pre-fixture store, CEF, DOM and global browser stubs.
        }
    })
})
