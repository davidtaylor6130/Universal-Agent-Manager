import { act, type ReactNode } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { MessageFrame, ToolCallModal, UserInputInlineCard } from './ToolCallViews'
import type { AcpPendingUserInput } from '../../store/useAppStore'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
let root: Root
let host: HTMLDivElement
let previousCef: typeof window.cefQuery
beforeEach(() => {
    vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
    previousCef = window.cefQuery
    host = document.createElement('div')
    document.body.appendChild(host)
    root = createRoot(host)
})
afterEach(() => {
    act(() => root.unmount())
    host.remove()
    window.cefQuery = previousCef
    vi.useRealTimers()
    vi.restoreAllMocks()
    vi.unstubAllGlobals()
})
const render = async (node: ReactNode) => { await act(async () => { root.render(node) }) }
const button = (label: string) => {
    const result = Array.from(document.querySelectorAll<HTMLButtonElement>('button')).find(item => item.textContent === label || item.getAttribute('aria-label') === label)
    if (!result) throw new Error(`Missing button: ${label}`)
    return result
}
const click = async (label: string) => { await act(async () => { button(label).click() }) }
const enter = async (value: string) => {
    const input = document.querySelector<HTMLInputElement>('.qr-text-answer input')!
    await act(async () => {
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, value)
        input.dispatchEvent(new Event('input', { bubbles: true }))
    })
}
const tool = { id: 'tool-1', title: 'Inspect files', kind: 'shell', status: 'completed', content: '', contentDeferred: true }
const request: AcpPendingUserInput = {
    requestId: 'request-1', itemId: 'input-1', status: 'pending', questions: [
        { id: 'scope', header: 'Scope', question: 'Which scope?', isOther: true, isSecret: false, options: [{ label: 'Focused', description: 'One fix' }, { label: 'Broad', description: 'All changes' }] },
        { id: 'secret', header: 'Secret', question: 'What secret?', isOther: false, isSecret: true, options: [] },
    ],
}

describe('production tool details', () => {
    it('pages real output, keeps metadata in Details, and copies only loaded text including from Details', async () => {
        const offsets: number[] = []
        const copied: string[] = []
        window.cefQuery = ({ request, onSuccess }) => {
            const call = JSON.parse(request)
            if (call.action === 'writeClipboardText') { copied.push(call.payload.text); onSuccess('{}'); return }
            const offset = call.payload.offset
            offsets.push(offset)
            onSuccess(JSON.stringify(offset === 0
                ? { content: 'first\n', offset: 0, nextOffset: 6, previousOffset: 0, lastOffset: 20, totalBytes: 25, hasPrevious: false, hasMore: true }
                : { content: 'last\n', offset: 20, nextOffset: 25, previousOffset: 6, lastOffset: 20, totalBytes: 25, hasPrevious: true, hasMore: false }))
        }
        await render(<ToolCallModal tool={tool} chatId="chat-1" onClose={vi.fn()} />)
        expect(document.querySelector('[role="tabpanel"]')?.textContent).not.toContain('tool-1')
        expect(document.querySelectorAll('[role="tab"]')).toHaveLength(2)
        await click('Load latest')
        expect(document.querySelector('pre')?.textContent).toContain('14 bytes not loaded')
        await click('Details')
        expect(document.querySelector('[role="tabpanel"]')?.textContent).toContain('tool-1')
        await click('Copy loaded output')
        expect(copied).toEqual(['first\nlast\n'])
        expect(offsets).toEqual([0, Number.MAX_SAFE_INTEGER])
        await click('Output')
        expect(document.querySelector('pre')?.textContent).toContain('first\n')
    })

    it('does not copy load errors or placeholders, and retries the failed page', async () => {
        let attempts = 0
        const writes: string[] = []
        window.cefQuery = ({ request, onSuccess, onFailure }) => {
            const call = JSON.parse(request)
            if (call.action === 'writeClipboardText') { writes.push(call.payload.text); onSuccess('{}'); return }
            if (++attempts === 1) { onFailure(500, 'Output unavailable'); return }
            onSuccess(JSON.stringify({ content: 'recovered', offset: 0, nextOffset: 9, previousOffset: 0, lastOffset: 0, totalBytes: 9, hasPrevious: false, hasMore: false }))
        }
        vi.spyOn(console, 'error').mockImplementation(() => {})
        await render(<ToolCallModal tool={tool} chatId="chat-1" onClose={vi.fn()} />)
        await click('Copy loaded output')
        expect(writes).toEqual([])
        await click('Retry')
        await click('Copy loaded output')
        expect(writes).toEqual(['recovered'])
    })

    it('follows real live pages, pauses for earlier output, and retains pages after completion', async () => {
        vi.useFakeTimers()
        const offsets: number[] = []
        window.cefQuery = ({ request, onSuccess }) => {
            const offset = JSON.parse(request).payload.offset
            offsets.push(offset)
            onSuccess(JSON.stringify({ content: offset === 0 ? 'start' : 'tail', offset: offset === 0 ? 0 : 5, nextOffset: offset === 0 ? 5 : 9, previousOffset: 0, lastOffset: 5, totalBytes: 9, hasPrevious: offset !== 0, hasMore: offset === 0 }))
        }
        const props = { chatId: 'chat-1', onClose: vi.fn() }
        await render(<ToolCallModal {...props} tool={{ ...tool, status: 'running' }} />)
        await act(async () => { vi.advanceTimersByTime(1500) })
        expect(offsets).toEqual([Number.MAX_SAFE_INTEGER, Number.MAX_SAFE_INTEGER])
        await click('Load earlier')
        await act(async () => { vi.advanceTimersByTime(3000) })
        expect(offsets).toHaveLength(3)
        await render(<ToolCallModal {...props} tool={tool} />)
        expect(document.querySelector('pre')?.textContent).toBe('starttail')
        expect(offsets).toHaveLength(3)
    })

    it('loads only a referenced transcript and preserves interrupted resume and chat callbacks', async () => {
        const calls: string[] = []
        const onOpen = vi.fn()
        window.cefQuery = ({ request, onSuccess }) => {
            const call = JSON.parse(request)
            calls.push(call.action)
            if (call.action === 'resumeAgentRun') { expect(call.payload).toEqual({ runId: 'old-run' }); onSuccess(JSON.stringify({ runId: 'fresh-run' })); return }
            expect(call.payload).toEqual({ chatId: 'chat-1', transcriptChatId: 'child-1' })
            onSuccess(JSON.stringify({ runId: 'old-run', title: 'Agent report', status: 'interrupted', executionCapability: 'local', messages: [{ role: 'assistant', content: 'Actual report' }, { role: 5, content: 'invalid' }] }))
        }
        await render(<ToolCallModal tool={{ ...tool, contentDeferred: false, isSubAgent: true, content: '{"transcriptChatId":"child-1"}' }} chatId="chat-1" onClose={vi.fn()} onOpenSubAgent={onOpen} />)
        expect(calls).toEqual([])
        await click('Transcript')
        await click('View managed agent transcript')
        expect(document.querySelector('[role="tabpanel"]')?.textContent).toContain('Actual report')
        expect(document.querySelector('[role="tabpanel"]')?.textContent).not.toContain('invalid')
        await click('Resume as fresh run')
        await click('Resume as fresh run')
        expect(calls.filter(action => action === 'resumeAgentRun')).toHaveLength(1)
        await click('Details')
        await click('Open chat')
        expect(onOpen).toHaveBeenCalledOnce()
    })

    it('supports tab arrows, traps Tab and restores focus after dismissal', async () => {
        const trigger = document.createElement('button')
        document.body.appendChild(trigger)
        trigger.focus()
        const onClose = vi.fn(() => root.render(null))
        await render(<ToolCallModal tool={{ ...tool, contentDeferred: false }} onClose={onClose} />)
        act(() => button('Output').dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true })))
        expect(document.activeElement).toBe(button('Details'))
        expect(button('Details').getAttribute('aria-selected')).toBe('true')
        const panel = document.querySelector<HTMLElement>('[role="tabpanel"]')!
        panel.focus()
        act(() => panel.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', bubbles: true })))
        expect(document.activeElement).toBe(button('Copy loaded output'))
        act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' })))
        expect(onClose).toHaveBeenCalledOnce()
        expect(document.activeElement).toBe(trigger)
        trigger.remove()
    })
})

describe('production sequential questions', () => {
    it('accepts a no-options free-text answer without inventing a default', async () => {
        const onResolve = vi.fn().mockResolvedValue(true)
        const input = { ...request, questions: [{ ...request.questions[1], id: 'note', header: 'Note', question: 'Any detail?', isSecret: false }] }
        await render(<UserInputInlineCard input={input} onResolve={onResolve} />)
        expect(document.querySelectorAll('input[type="radio"]')).toHaveLength(0)
        expect(document.querySelector('.qr-text-answer input')?.getAttribute('type')).toBe('text')
        expect(button('Submit answers').disabled).toBe(true)
        await enter('   ')
        expect(button('Submit answers').disabled).toBe(true)
        expect(onResolve).not.toHaveBeenCalled()
        await enter('  Only update the docs  ')
        await click('Submit answers')
        expect(onResolve).toHaveBeenCalledExactlyOnceWith('request-1', { note: ['Only update the docs'] })
    })

    it('keeps options and custom drafts across navigation/dismissal and retries the real payload once', async () => {
        let resolve!: (accepted: boolean) => void
        const onResolve = vi.fn(() => new Promise<boolean>(done => { resolve = done }))
        await render(<UserInputInlineCard input={request} onResolve={onResolve} />)
        expect(document.querySelector('legend')?.textContent).toBe('Which scope?')
        expect(button('Next').disabled).toBe(true)
        await enter('  custom scope  ')
        act(() => document.querySelector<HTMLInputElement>('input[type="radio"]')!.click())
        await click('Next')
        expect(document.querySelector('.qr-text-answer input')?.getAttribute('type')).toBe('password')
        await enter('  a secret  ')
        await click('Back')
        expect(document.querySelector<HTMLInputElement>('.qr-text-answer input')?.value).toBe('  custom scope  ')
        expect(document.querySelector<HTMLInputElement>('input[type="radio"]')?.checked).toBe(true)
        await click('Close questions')
        expect(document.activeElement).toBe(button('Answer questions'))
        await click('Answer questions')
        await click('Next')
        expect(document.querySelector<HTMLInputElement>('.qr-text-answer input')?.value).toBe('  a secret  ')
        await click('Submit answers')
        await click('Submitting…')
        expect(onResolve).toHaveBeenCalledTimes(1)
        expect(onResolve).toHaveBeenLastCalledWith('request-1', { scope: ['Focused'], secret: ['a secret'] })
        expect(button('Back').disabled).toBe(true)
        await act(async () => resolve(false))
        expect(document.querySelector('[role="alert"]')?.textContent).toContain('Try again')
        await click('Retry submission')
        expect(onResolve).toHaveBeenCalledTimes(2)
        await act(async () => resolve(true))
        expect(document.querySelector('[role="dialog"]')).toBeNull()
        expect(button('Answers submitted').disabled).toBe(true)
    })

    it('clears drafts for a new request with reused question IDs and ignores an old completion', async () => {
        let resolve!: (accepted: boolean) => void
        const onResolve = vi.fn(() => new Promise<boolean>(done => { resolve = done }))
        const input = { ...request, questions: [request.questions[1]] }
        await render(<UserInputInlineCard input={input} onResolve={onResolve} />)
        await enter('old answer')
        await click('Submit answers')
        await render(<UserInputInlineCard input={{ ...input, requestId: 'request-2' }} onResolve={onResolve} />)
        expect(document.querySelector<HTMLInputElement>('.qr-text-answer input')?.value).toBe('')
        await enter('new answer')
        await act(async () => resolve(true))
        expect(document.querySelector<HTMLInputElement>('.qr-text-answer input')?.value).toBe('new answer')
        expect(button('Submit answers').disabled).toBe(false)
    })

    it('preserves drafts through thrown submission failures and keeps recovery callbacks', async () => {
        const onCancel = vi.fn()
        const onStop = vi.fn()
        const onResolve = vi.fn().mockRejectedValue(new Error('offline'))
        await render(<UserInputInlineCard input={{ ...request, questions: [request.questions[1]] }} onResolve={onResolve} waitIsStale waitSeconds={150} onCancelTurn={onCancel} onStopRuntime={onStop} />)
        await enter('secret')
        await click('Submit answers')
        expect(document.querySelector('[role="alert"]')?.textContent).toContain('Could not submit')
        await click('Close questions')
        await click('Answer questions')
        expect(document.querySelector<HTMLInputElement>('.qr-text-answer input')?.value).toBe('secret')
        await click('Cancel turn')
        await click('Stop runtime')
        expect(onCancel).toHaveBeenCalledOnce()
        expect(onStop).toHaveBeenCalledOnce()
    })
})

it('forwards message props and actions to the approved conversation turn', async () => {
    const onEdit = vi.fn()
    const onNext = vi.fn()
    await render(<MessageFrame role="assistant" assistantLabel="Claude" copyText="answer" onEdit={onEdit} streaming branchLabel="Branch" branchNavigation={{ current: 1, total: 2, onPrevious: vi.fn(), onNext }}>Answer body</MessageFrame>)
    expect(document.querySelector('.conversation-turn')?.getAttribute('aria-label')).toBe('Claude')
    expect(document.querySelector('.conversation-turn')?.getAttribute('data-streaming')).toBe('true')
    await click('Edit message in new branch')
    await click('Next message branch')
    expect(onEdit).toHaveBeenCalledOnce()
    expect(onNext).toHaveBeenCalledOnce()
})
