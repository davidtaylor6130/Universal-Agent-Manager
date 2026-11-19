import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { UserInputInlineCard } from './QuestionInput'
import { useAppStore, type AcpPendingUserInput, type AcpSecretRequest } from '../../store/useAppStore'
import { sanitizePendingUserInput } from '../../store/cpp/sanitizers'

const bridge = vi.hoisted(() => ({ companion: false }))
vi.mock('../../ipc/cefBridge', async (original) => ({ ...await original<typeof import('../../ipc/cefBridge')>(), isCompanionContext: () => bridge.companion }))

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const CANARY = 'ghp_CANARY_7Hq2mZcLx41TfVwB9rN0'
const base: AcpSecretRequest = {
    label: 'GitHub personal access token', reason: 'To push the v5.1.0 tag.', placeholder: 'ghp_…', url: '', provider: 'Codex',
    host: null, localPlatform: 'macos', planOptIn: false, keychain: { available: true, saved: false },
}
const pending = (secretRequest: Partial<AcpSecretRequest> = {}): AcpPendingUserInput => ({
    requestId: 'uam-control:cap:req-1', itemId: '', status: 'pending', attentionKind: 'question',
    questions: [{ id: 'secretRequest', header: 'Secret', question: 'Codex asks for a secret: GitHub personal access token. Answer it on the computer.', options: [], isOther: false, isSecret: false }],
    secretRequest: { ...base, ...secretRequest },
})

let root: Root
let host: HTMLDivElement
let previousCef: typeof window.cefQuery
beforeEach(() => {
    previousCef = window.cefQuery
    bridge.companion = false
    host = document.createElement('div')
    document.body.appendChild(host)
    root = createRoot(host)
})
afterEach(() => {
    act(() => root.unmount())
    host.remove()
    window.cefQuery = previousCef
    vi.restoreAllMocks()
})

const onResolve = vi.fn(async () => true)
const render = async (input: AcpPendingUserInput, resolve = onResolve) => {
    await act(async () => { root.render(<UserInputInlineCard input={input} sessionId="chat-1" onResolve={resolve} />) })
}
const field = () => host.querySelector<HTMLInputElement>('.secret-card__field input')!
const button = (name: string) => {
    const found = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(item => (item.querySelector('.uam-btn__label')?.textContent ?? item.textContent) === name || item.getAttribute('aria-label') === name)
    if (!found) throw new Error(`Missing button: ${name}`)
    return found
}
const click = async (name: string) => { await act(async () => { button(name).click() }) }
const type = async (value: string) => {
    await act(async () => {
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(field(), value)
        field().dispatchEvent(new Event('input', { bubbles: true }))
    })
}
const key = async (name: string) => {
    await act(async () => { field().dispatchEvent(new KeyboardEvent('keydown', { key: name, bubbles: true, cancelable: true })) })
}
const settle = () => act(async () => { await new Promise(resolve => setTimeout(resolve, 120)) })

describe('SecretRequestCard', () => {
    it('renders the local card with a masked, unassisted field and the local assurance', async () => {
        await render(pending())
        expect(host.querySelector('h2')?.textContent).toBe('Codex asks for a secret')
        expect(host.textContent).toContain('This Mac')
        expect(host.querySelector('label')?.textContent).toBe('GitHub personal access token')
        expect(host.querySelector('.secret-card__reason')?.textContent).toBe('Codex: To push the v5.1.0 tag.')
        const input = field()
        expect(input.type).toBe('password')
        expect(input.getAttribute('autocomplete')).toBe('off')
        expect(input.getAttribute('autocapitalize')).toBe('off')
        expect(input.getAttribute('autocorrect')).toBe('off')
        expect(input.getAttribute('spellcheck')).toBe('false')
        expect(document.activeElement).not.toBe(input)
        expect(host.textContent).toContain('Stays on this Mac. The agent gets a one-read file path, never the value. Never shown to the model or saved in chat history.')
        expect(host.querySelector('input[type="checkbox"]')?.getAttribute('aria-label')).toBe('Save in Keychain')
        expect(host.querySelector('[aria-live="polite"]')?.textContent).toBe('Codex asks for a secret: GitHub personal access token.')
    })

    it('renders the sign-in link, SSH Linux and SSH Windows variants', async () => {
        const open = vi.spyOn(window, 'open').mockReturnValue(null)
        await render(pending({ url: 'https://platform.openai.com/api-keys' }))
        expect(host.textContent).toContain('Opens platform.openai.com in your browser')
        await click('Get a key')
        expect(open).toHaveBeenCalledWith('https://platform.openai.com/api-keys', '_blank', 'noopener')

        await render(pending({ host: { name: 'build-box', platform: 'linux' } }))
        expect(host.textContent).toMatch(/build-box\s·\sLinux/)
        expect(host.textContent).toContain('Sent over the UAM connection to build-box and written to a private pipe only your account can read, removed after one read.')
        expect(host.querySelector('input[type="checkbox"]')).toBeNull()

        await render(pending({ host: { name: 'win-build', platform: 'windows' } }))
        expect(host.textContent).toContain('Sent over the UAM connection to win-build and written to a file only your account can read, deleted within 60 seconds.')
        // Windows secrets are one-time: no Save, no saved-secret actions, even if the backend claimed otherwise.
        await render(pending({ localPlatform: 'windows', keychain: { available: true, saved: true } }))
        expect(host.textContent).toContain('This PC')
        expect(host.textContent).toContain('deleted within 60 seconds')
        expect(host.textContent).not.toContain('Save in')
        expect(host.textContent).not.toContain('Use saved secret')
        expect(host.querySelector('input[type="checkbox"]')).toBeNull()
    })

    it('keeps Submit disabled until something is typed and warns about short values', async () => {
        await render(pending())
        expect(button('Submit').getAttribute('aria-disabled')).toBe('true')
        await type('abc')
        expect(button('Submit').getAttribute('aria-disabled')).toBeNull()
        expect(host.textContent).toContain("Values under 6 characters can't be hidden if the agent prints them.")
        await type('abcdef')
        await settle()
        expect(host.textContent).not.toContain('Values under 6 characters')
    })

    it('toggles show and hide with aria-pressed', async () => {
        await render(pending())
        expect(button('Show secret').getAttribute('aria-pressed')).toBe('false')
        await click('Show secret')
        expect(field().type).toBe('text')
        expect(button('Hide secret').getAttribute('aria-pressed')).toBe('true')
        await click('Hide secret')
        expect(field().type).toBe('password')
    })

    it('submits on Enter through the bridge without the value reaching the store or the stub log', async () => {
        const debug = vi.spyOn(console, 'debug').mockImplementation(() => {})
        await render(pending())
        await type(CANARY)
        await key('Escape')
        expect(onResolve).not.toHaveBeenCalled()
        await key('Enter')
        await settle()
        const logged = JSON.stringify(debug.mock.calls)
        expect(logged).toContain('submitSecretRequest')
        expect(logged).not.toContain(CANARY)
        expect(JSON.stringify(useAppStore.getState())).not.toContain(CANARY)
        expect(field().value).toBe('')
        expect(onResolve).not.toHaveBeenCalled()
    })

    it('sends value and save flag, and the saved-secret actions', async () => {
        const sent: unknown[] = []
        window.cefQuery = ({ request, onSuccess }) => { sent.push(JSON.parse(request)); onSuccess('{}') }
        await render(pending({ keychain: { available: true, saved: true } }))
        await type(CANARY)
        await act(async () => { host.querySelector<HTMLInputElement>('input[type="checkbox"]')!.click() })
        await click('Submit')
        expect(sent).toMatchObject([{ action: 'submitSecretRequest', payload: { chatId: 'chat-1', requestId: 'uam-control:cap:req-1', value: CANARY, save: true } }])
        expect(JSON.stringify(useAppStore.getState())).not.toContain(CANARY)

        sent.length = 0
        await render({ ...pending({ keychain: { available: true, saved: true } }), requestId: 'uam-control:cap:req-2' })
        await click('Use saved secret')
        expect(sent).toMatchObject([{ action: 'submitSecretRequest', payload: { requestId: 'uam-control:cap:req-2', useSaved: true } }])
        expect((sent[0] as { payload: object }).payload).not.toHaveProperty('value')

        sent.length = 0
        await render({ ...pending({ keychain: { available: true, saved: true } }), requestId: 'uam-control:cap:req-3' })
        await click('Forget saved')
        expect(sent).toMatchObject([{ action: 'forgetSavedSecret', payload: { chatId: 'chat-1', requestId: 'uam-control:cap:req-3' } }])
    })

    it('keeps the typed value and shows the error as an alert when delivery fails', async () => {
        vi.spyOn(console, 'error').mockImplementation(() => {})
        window.cefQuery = ({ onFailure }) => onFailure(500, 'build-box is not connected.')
        await render(pending({ host: { name: 'build-box', platform: 'linux' } }))
        await type(CANARY)
        await click('Submit')
        const alert = host.querySelector('[role="alert"]')
        expect(alert?.textContent).toContain('build-box is not connected.')
        expect(field().value).toBe(CANARY)
        expect(field().getAttribute('aria-invalid')).toBe('true')
        expect(button('Retry')).toBeTruthy()
        expect(JSON.stringify(useAppStore.getState())).not.toContain(CANARY)
    })

    it('cancels through the UAM answer and wipes the field', async () => {
        await render(pending())
        await type(CANARY)
        await click('Cancel')
        expect(onResolve).toHaveBeenCalledWith('uam-control:cap:req-1', { secretRequest: ['Deny'] })
        expect(field().value).toBe('')
    })

    it('clears the field on unmount', async () => {
        await render(pending())
        await type(CANARY)
        const input = field()
        await act(async () => { root.render(<div />) })
        expect(input.value).toBe('')
    })

    it('asks for Plan mode opt-in with Deny and Allow once and no field', async () => {
        await render(pending({ planOptIn: true }))
        expect(host.querySelector('h2')?.textContent).toBe('Codex asks for a secret in Plan mode')
        expect(field()).toBeNull()
        expect(host.textContent).toContain("Plan mode doesn't allow secrets.")
        await click('Allow once')
        expect(onResolve).toHaveBeenLastCalledWith('uam-control:cap:req-1', { secretRequest: ['Allow'] })
        await render({ ...pending({ planOptIn: true }), requestId: 'uam-control:cap:req-4' })
        await click('Deny')
        expect(onResolve).toHaveBeenLastCalledWith('uam-control:cap:req-4', { secretRequest: ['Deny'] })
    })

    it('shows a read-only notice on the phone', async () => {
        bridge.companion = true
        await render(pending())
        expect(host.textContent).toBe('Codex asks for a secret: GitHub personal access token. Answer it on the computer.')
        expect(host.querySelector('input')).toBeNull()
        expect(host.querySelector('button')).toBeNull()
    })

    it('never offers a free-text answer for a malformed secret request', async () => {
        const { secretRequest: _dropped, ...input } = pending()
        await render(input)
        expect(host.querySelector('input')).toBeNull()
        await click('Cancel request')
        expect(onResolve).toHaveBeenLastCalledWith('uam-control:cap:req-1', { secretRequest: ['Deny'] })
    })
})

describe('sanitizePendingUserInput secretRequest', () => {
    const raw = (secretRequest: unknown) => ({ ...pending(), secretRequest })
    it('keeps a valid request', () => {
        expect(sanitizePendingUserInput(raw(base))?.secretRequest).toEqual(base)
        const remote = { ...base, url: 'https://github.com/settings/tokens', host: { name: 'build-box', platform: 'linux' } }
        expect(sanitizePendingUserInput(raw(remote))?.secretRequest).toEqual(remote)
    })
    it('drops malformed requests', () => {
        for (const bad of [null, 'x', { ...base, label: '' }, { ...base, url: 'http://example.com' }, { ...base, url: 'javascript:alert(1)' },
            { ...base, host: { name: '' , platform: 'linux' } }, { ...base, host: undefined }, { ...base, planOptIn: 'yes' },
            { ...base, keychain: { available: true } }, { ...base, provider: 7 }])
            expect(sanitizePendingUserInput(raw(bad))).not.toHaveProperty('secretRequest')
    })
})
