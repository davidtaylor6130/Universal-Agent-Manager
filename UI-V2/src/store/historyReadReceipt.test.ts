import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from './useAppStore'
import type { AcceptedHistoryPage } from './storeTypes'
import type { Session } from '../types/session'

type Envelope = { action: string; requestId: string; payload?: Record<string, unknown> }
type Held = { envelope: Envelope; done: boolean; finish: (data?: Record<string, unknown>) => void }
let serial = 0
let previous: ReturnType<typeof useAppStore.getState>, previousCef: Window['cefQuery']
let session: Session, held: Held[], promises: Promise<unknown>[], unsubscribe: (() => void) | undefined
const page = () => ({ chatId: session.id, messagesDigest: 'page-hash', startIndex: 48, totalCount: 50,
    messages: [
        { role: 'user', content: 'Actual saved user row', createdAt: '2026-10-23T09:00:00Z' },
        { role: 'assistant', content: 'Actual saved answer row', createdAt: '2026-10-23T09:00:01Z' },
    ] })
const read = () => {
    const action = useAppStore.getState().loadSessionMessages
    expect(action).toBe(previous.loadSessionMessages)
    expect(vi.isMockFunction(action)).toBe(false)
    const before = held.length
    const result = action(session.id)
    expect(result).toBeInstanceOf(Promise)
    expect(held).toHaveLength(before + 1)
    expect(held[before].envelope).toMatchObject({ action: 'getChatMessages', requestId: expect.any(String),
        payload: { chatId: session.id, limit: 50, deferToolCallContent: true } })
    const promise = Promise.resolve(result); promises.push(promise)
    return { pending: held[before], promise }
}
const receipt = (value: unknown): AcceptedHistoryPage => {
    expect(value).toBeTruthy()
    expect(typeof value).toBe('object')
    expect(value).toMatchObject({ chatId: session.id, birth: session.createdAt.getTime(),
        summaryDigest: 'summary-token', summaryCount: 50, messagesDigest: 'page-hash', startIndex: 48, totalCount: 50 })
    return value as AcceptedHistoryPage
}
const accepted = async () => {
    const request = read()
    expect(request.pending.envelope.payload?.messagesDigest).toBe('')
    request.pending.finish(page())
    return { request, value: receipt(await request.promise) }
}

beforeEach(() => {
    previous = useAppStore.getState(); previousCef = window.cefQuery
    held = []; promises = []; unsubscribe = undefined
    const createdAt = new Date('2026-10-23T08:00:00Z')
    session = { id: `history-receipt-${++serial}`, name: 'Receipt fixture', providerId: 'codex-cli', folderId: null,
        viewMode: 'chat', createdAt, updatedAt: createdAt, executionHostId: 'local', workspaceDirectory: '/fixture/receipt',
        workspaceWorktreeDirectory: '', importedReadOnly: true, messagesDigest: 'summary-token', messageCount: 50 }
    window.cefQuery = ({ request, onSuccess }) => {
        const envelope = JSON.parse(request) as Envelope
        if (envelope.action !== 'getChatMessages') {
            onSuccess(JSON.stringify({ ok: envelope.action === 'releaseChatMessages', data: {}, requestId: envelope.requestId }))
            return
        }
        const item: Held = { envelope, done: false, finish: data => {
            if (item.done) return
            item.done = true
            onSuccess(JSON.stringify(data ? { ok: true, data, requestId: envelope.requestId }
                : { ok: false, error: 'Fixture cleanup retires held read', requestId: envelope.requestId }))
        } }
        held.push(item)
    }
    useAppStore.setState({ sessions: [session], messages: { [session.id]: [] },
        historyStartIndexBySessionId: { [session.id]: 0 }, chatHistoryErrorBySessionId: {},
        chatHistoryReadBySessionId: {}, chatSearchNavigation: null,
        acpBindingBySessionId: {}, cliBindingBySessionId: {} })
})
afterEach(async () => {
    try {
        unsubscribe?.(); unsubscribe = undefined
        held.forEach(item => item.finish())
        await Promise.allSettled(promises)
        // Use the original real action while the private fake bridge is still installed.
        previous.unloadSessionMessages(session.id)
        expect(useAppStore.getState().chatHistoryPageBySessionId[session.id]).toBeUndefined()
        expect(held.every(item => item.done)).toBe(true)
    } finally {
        window.cefQuery = previousCef
        useAppStore.setState(previous, true)
    }
})

describe('real saved-history acceptance receipts, authored UNRUN', () => {
    it('returns an accepted nonstreaming tail with the exact adopted array and row identities', async () => {
        const { request, value } = await accepted()
        const rows = useAppStore.getState().messages[session.id]
        expect(value.requestId).toBe(request.pending.envelope.requestId)
        expect(value.messages).toBe(rows)
        expect(useAppStore.getState().chatHistoryPageBySessionId[session.id]).toBe(value)
        expect(value.session).toBe(session)
        expect(value.messages).toHaveLength(2)
        expect(value.messages[0]).toBe(rows[0]); expect(value.messages[1]).toBe(rows[1])
        expect(value.messages.map(row => row.content)).toEqual(['Actual saved user row', 'Actual saved answer row'])
        expect(value.messages.some(row => row.isStreaming)).toBe(false)
        expect(useAppStore.getState().historyStartIndexBySessionId[session.id]).toBe(48)
        useAppStore.setState(state => ({ messages: { ...state.messages, [session.id]: [] } }))
        expect(value.messages).toBe(rows)
        expect(value.messages[1].content).toBe('Actual saved answer row')
    })

    it('returns no receipt when concurrent message replacement makes the guarded updater refuse adoption', async () => {
        const { pending, promise } = read()
        const replacement = [{ id: 'concurrent-row', sessionId: session.id, role: 'user' as const,
            content: 'Concurrent retained evidence', createdAt: new Date() }]
        useAppStore.setState(state => ({ messages: { ...state.messages, [session.id]: replacement } }))
        pending.finish(page())
        expect(await promise).toBeUndefined()
        expect(useAppStore.getState().messages[session.id]).toBe(replacement)
        expect(useAppStore.getState().historyStartIndexBySessionId[session.id]).toBe(0)
    })

    it('returns no receipt if a real synchronous subscriber replaces the newly adopted array', async () => {
        let replaced = false
        unsubscribe = useAppStore.subscribe(state => {
            if (!replaced && state.messages[session.id]?.[0]?.content === 'Actual saved user row') {
                replaced = true
                useAppStore.setState(current => ({ messages: { ...current.messages, [session.id]: [] } }))
            }
        })
        const { pending, promise } = read()
        pending.finish(page())
        expect(await promise).toBeUndefined()
        expect(replaced).toBe(true)
        expect(useAppStore.getState().messages[session.id]).toEqual([])
    })

    it.each(['unload', 'new birth', 'superseded'] as const)('returns no receipt for a held read after %s', async change => {
        const old = read()
        let newer: ReturnType<typeof read> | undefined
        if (change === 'unload') previous.unloadSessionMessages(session.id)
        if (change === 'new birth') useAppStore.setState({ sessions: [{ ...session, createdAt: new Date(session.createdAt.getTime() + 1) }] })
        if (change === 'superseded') newer = read()
        const beforeReply = useAppStore.getState().messages[session.id]
        old.pending.finish(page())
        expect(await old.promise).toBeUndefined()
        expect(useAppStore.getState().messages[session.id]).toBe(beforeReply)
        if (newer) {
            expect(newer.pending.done).toBe(false)
            newer.pending.finish(page())
            receipt(await newer.promise)
        }
    })

    it('returns a conditional unchanged receipt only for the positively loaded cached page', async () => {
        const first = await accepted()
        const next = read()
        expect(next.pending.envelope.payload?.messagesDigest).toBe('page-hash')
        next.pending.finish({ chatId: session.id, unchanged: true, messagesDigest: 'page-hash', startIndex: 48, totalCount: 50 })
        const value = receipt(await next.promise)
        expect(value.requestId).toBe(next.pending.envelope.requestId)
        expect(value.requestId).not.toBe(first.value.requestId)
        expect(value.messages).toBe(first.value.messages)
        expect(value.messages).toBe(useAppStore.getState().messages[session.id])
    })

    it('refuses an unchanged receipt carrying a different page digest without replacing cached rows', async () => {
        const first = await accepted()
        const next = read()
        expect(next.pending.envelope.payload?.messagesDigest).toBe('page-hash')
        next.pending.finish({ chatId: session.id, unchanged: true, messagesDigest: 'foreign-page', startIndex: 48, totalCount: 50 })
        expect(await next.promise).toBeUndefined()
        expect(useAppStore.getState().messages[session.id]).toBe(first.value.messages)
    })
})

describe('accepted evidence versus later pending reads', () => {
    it('keeps a positively adopted current page receipt when a subscriber begins a later read without replacing its rows', async () => {
        let later: ReturnType<typeof read> | undefined
        unsubscribe = useAppStore.subscribe(state => {
            if (!later && state.messages[session.id]?.[0]?.content === 'Actual saved user row') {
                // Prearm against synchronous store notifications before beginning the second real read.
                unsubscribe?.(); unsubscribe = undefined
                later = read()
            }
        })
        const first = read()
        first.pending.finish(page())
        const value = receipt(await first.promise)
        expect(later).toBeDefined()
        expect(later!.pending.done).toBe(false)
        expect(value.messages).toBe(useAppStore.getState().messages[session.id])
        later!.pending.finish()
        expect(await later!.promise).toBe(false)
        expect(value.messages).toBe(useAppStore.getState().messages[session.id])
        expect(value.messages.map(row => row.content)).toEqual(['Actual saved user row', 'Actual saved answer row'])
    })
})
