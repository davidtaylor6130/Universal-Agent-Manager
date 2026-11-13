import { useCallback, useEffect, useId, useRef, useState, type KeyboardEvent } from 'react'
import { ExternalLink, Eye, EyeOff, KeyRound, Laptop, Lock, Server, ShieldCheck, TriangleAlert } from 'lucide-react'
import { Button, IconButton, Notice, Reveal, Switch } from '../ui'
import { useAppStore, type AcpPendingUserInput, type AcpSecretRequest, type AcpUserInputAnswers } from '../../store/useAppStore'
import { isCompanionContext } from '../../ipc/cefBridge'
import './SecretRequestCard.css'

type Props = {
    input: AcpPendingUserInput
    sessionId?: string
    onResolve: (requestId: string, answers: AcpUserInputAnswers) => Promise<boolean>
}
type Busy = '' | 'submit' | 'saved' | 'deny' | 'allow' | 'forget'

const PLATFORM_NAMES: Record<string, string> = { linux: 'Linux', windows: 'Windows', macos: 'macOS' }

function assurance(host: AcpSecretRequest['host'], localPlatform: string)
{
    const delivery = !host ? (localPlatform === 'windows'
            ? 'Stays on this PC in a file only your account can read, deleted within 60 seconds. The agent gets its path, never the value.'
            : 'Stays on this Mac. The agent gets a one-read file path, never the value.')
        : host.platform === 'windows' ? `Sent over the UAM connection to ${host.name} and written to a file only your account can read, deleted within 60 seconds.`
        : `Sent over the UAM connection to ${host.name} and written to a private pipe only your account can read, removed after one read.`
    return `${delivery} Never shown to the model or saved in chat history.`
}

function hostOf(url: string)
{
    try { return new URL(url).host } catch { return '' }
}

/**
 * UAM-control secret_request. The typed value lives only in the input element and the outgoing request:
 * never in React or Zustand state, and the field is cleared after a delivered submit and on unmount.
 */
export function SecretRequestCard({ input, sessionId, onResolve }: Props)
{
    const request = input.secretRequest
    const id = useId()
    const fieldRef = useRef<HTMLInputElement | null>(null)
    // Stable ref callback: React calls it with null on unmount, which wipes the field.
    const attachField = useCallback((node: HTMLInputElement | null) => {
        if (!node && fieldRef.current) fieldRef.current.value = ''
        fieldRef.current = node
    }, [])
    const [hasValue, setHasValue] = useState(false)
    const [shortValue, setShortValue] = useState(false)
    const [revealed, setRevealed] = useState(false)
    const [save, setSave] = useState(false)
    const [busy, setBusy] = useState<Busy>('')
    const [error, setError] = useState('')
    const [announcement, setAnnouncement] = useState('')
    const submitSecretRequest = useAppStore(state => state.submitSecretRequest)
    const forgetSavedSecret = useAppStore(state => state.forgetSavedSecret)
    const mountedRef = useRef(true)
    useEffect(() => { mountedRef.current = true; return () => { mountedRef.current = false } }, [])
    // Announced, never focused: the card must not take the caret from the composer.
    useEffect(() => {
        if (request) setAnnouncement(`${request.provider} asks for a secret: ${request.label}.`)
    }, [request?.provider, request?.label]) // eslint-disable-line react-hooks/exhaustive-deps
    // Allow and Forget wait for native to push the changed request; a field shown by Allow takes focus, as the user just acted.
    const planOptIn = request?.planOptIn
    const wasPlan = useRef(planOptIn)
    useEffect(() => {
        setBusy(current => current === 'allow' || current === 'forget' ? '' : current)
        if (wasPlan.current && planOptIn === false) fieldRef.current?.focus()
        wasPlan.current = planOptIn
    }, [input, planOptIn])

    const clearField = () => {
        if (fieldRef.current) fieldRef.current.value = ''
        setHasValue(false)
        setShortValue(false)
        setRevealed(false)
    }
    const resolve = async (answer: 'Allow' | 'Deny', kind: Busy) => {
        if (busy) return
        setBusy(kind)
        setError('')
        if (answer === 'Deny') clearField()
        const accepted = await onResolve(input.requestId, { secretRequest: [answer] }).catch(() => false)
        if (!mountedRef.current) return
        if (!accepted) { setBusy(''); setError('UAM did not take the answer. Try again.') }
        else if (answer === 'Deny') setAnnouncement('Secret request cancelled.')
    }
    const submit = async (useSaved = false) => {
        const value = fieldRef.current?.value ?? ''
        if (busy || (!useSaved && !value)) return
        if (!sessionId) { setError('This chat is not available.'); return }
        setBusy(useSaved ? 'saved' : 'submit')
        setError('')
        const result = await submitSecretRequest(sessionId, input.requestId, useSaved ? { useSaved: true } : { value, ...(save ? { save: true } : {}) })
            .catch(() => ({ ok: false, error: '' }))
        if (!mountedRef.current) return
        // Delivered: stay busy until native removes the request. Failed: keep the entry for Retry.
        if (result.ok) { clearField(); setAnnouncement(`${request?.label ?? 'Secret'} provided.`); return }
        setBusy('')
        setError(result.error || 'The secret was not delivered.')
    }
    const forget = async () => {
        if (busy || !sessionId) return
        setBusy('forget')
        setError('')
        const result = await forgetSavedSecret(sessionId, input.requestId).catch(() => ({ ok: false, error: '' }))
        if (!mountedRef.current) return
        if (!result.ok) { setBusy(''); setError(result.error || 'The saved secret was not forgotten.') }
    }
    const onFieldInput = () => {
        const length = fieldRef.current?.value.length ?? 0
        setHasValue(length > 0)
        setShortValue(length > 0 && length < 6)
    }
    const onFieldKeyDown = (event: KeyboardEvent<HTMLInputElement>) => {
        if (event.nativeEvent.isComposing || event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return
        // Escape is swallowed so a stray key never cancels the request or the turn.
        if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation() }
        if (event.key !== 'Enter') return
        event.preventDefault()
        event.stopPropagation()
        void submit()
    }
    const live = <span className="sr-only" aria-live="polite">{announcement}</span>

    // The phone never takes a secret; a malformed request is not answerable here either.
    if (!request || isCompanionContext())
    {
        const text = request ? `${request.provider} asks for a secret: ${request.label}.` : 'UAM could not show this secret request.'
        return <div className="secret-card secret-card--readonly" data-testid="secret-request-card">
            <Notice tone="info" title={text} dismissible={false}>{request ? 'Answer it on the computer.' : null}</Notice>
            {!request && !isCompanionContext() && <Button size="sm" variant="ghost" loading={busy === 'deny'} onClick={() => void resolve('Deny', 'deny')}>Cancel request</Button>}
        </div>
    }

    const { provider, label, reason, placeholder, url, host, keychain, localPlatform } = request
    const thisComputer = localPlatform === 'windows' ? 'This PC' : 'This Mac'
    // Saving is macOS-only: on Windows every secret is one-time (any program of the user can read Credential Manager).
    const canSave = keychain.available && !host && localPlatform !== 'windows'
    const hasSaved = keychain.saved && !host && localPlatform !== 'windows'
    const headingId = `${id}-heading`
    const where = <span className="secret-card__where">
        {host
            ? <><Server size={13} aria-hidden /><span className="secret-card__mono">{host.name}</span>&nbsp;· {PLATFORM_NAMES[host.platform] ?? host.platform}</>
            : <><Laptop size={13} aria-hidden />{thisComputer}</>}
    </span>
    const reasonLine = reason.trim() && <p className="secret-card__reason" id={`${id}-reason`}><span>{provider}:</span> {reason}</p>
    const errorNotice = <Reveal open={Boolean(error)}>
        <Notice tone="error" title="Not delivered." dismissible={false}>{error}{busy === '' && hasValue ? ' Your entry is kept in the field until you retry or cancel.' : ''}</Notice>
    </Reveal>

    if (request.planOptIn) return <section className="qr-panel secret-card" data-variant="float" aria-labelledby={headingId} data-testid="secret-request-card">
        {live}
        <header className="qr-header">
            <Lock size={14} aria-hidden />
            <h2 id={headingId}>{provider} asks for a secret in Plan mode</h2>
            {where}
        </header>
        <div className="secret-card__body">
            <p className="secret-card__label">{label}</p>
            {reasonLine}
            <Notice tone="warning" title="Plan mode doesn't allow secrets." dismissible={false}>Allow this one request to show the field; the mode stays Plan.</Notice>
            {errorNotice}
        </div>
        <footer className="secret-card__foot">
            <span />
            <div className="secret-card__actions">
                <Button variant="ghost" disabled={Boolean(busy)} loading={busy === 'deny'} onClick={() => void resolve('Deny', 'deny')}>Deny</Button>
                <Button variant="secondary" disabled={Boolean(busy)} loading={busy === 'allow'} onClick={() => void resolve('Allow', 'allow')}>Allow once</Button>
            </div>
        </footer>
    </section>

    const linkHost = url ? hostOf(url) : ''
    const fieldBusy = busy === 'submit' || busy === 'saved'
    return <section className="qr-panel secret-card" data-variant="float" aria-labelledby={headingId} data-testid="secret-request-card">
        {live}
        <header className="qr-header">
            <KeyRound size={14} aria-hidden />
            <h2 id={headingId}>{provider} asks for a secret</h2>
            {where}
        </header>
        <div className="secret-card__body">
            <label className="secret-card__label" htmlFor={`${id}-field`}>{label}</label>
            {reasonLine}
            {linkHost && <div className="secret-card__link">
                <Button size="sm" variant="secondary" leadingIcon={<ExternalLink size={13} aria-hidden />} aria-describedby={`${id}-dest`}
                    onClick={() => window.open(url, '_blank', 'noopener')}>Get a key</Button>
                <small id={`${id}-dest`}>Opens <span className="secret-card__mono">{linkHost}</span> in your browser</small>
            </div>}
            {errorNotice}
            <div className="secret-card__field" data-invalid={error ? '' : undefined}>
                <input ref={attachField} id={`${id}-field`} type={revealed ? 'text' : 'password'} placeholder={placeholder}
                    autoComplete="off" autoCapitalize="off" autoCorrect="off" spellCheck={false} data-1p-ignore="" data-lpignore="true"
                    aria-describedby={[reason.trim() && `${id}-reason`, shortValue && `${id}-short`, `${id}-assure`].filter(Boolean).join(' ')}
                    aria-invalid={error ? true : undefined} readOnly={fieldBusy} onInput={onFieldInput} onKeyDown={onFieldKeyDown} />
                <IconButton size="sm" icon={revealed ? <EyeOff size={15} aria-hidden /> : <Eye size={15} aria-hidden />}
                    label={revealed ? 'Hide secret' : 'Show secret'} aria-pressed={revealed} onClick={() => setRevealed(value => !value)} />
            </div>
            <Reveal open={shortValue}>
                <p className="secret-card__short" id={`${id}-short`}><TriangleAlert size={12} aria-hidden />Values under 6 characters can't be hidden if the agent prints them.</p>
            </Reveal>
            {(canSave || hasSaved) && <div className="secret-card__opts">
                {canSave && <Switch label="Save in Keychain" checked={save} disabled={Boolean(busy)} onChange={event => setSave(event.target.checked)} />}
                {hasSaved && <>
                    <Button size="sm" variant="secondary" disabled={Boolean(busy) && busy !== 'saved'} loading={busy === 'saved'} onClick={() => void submit(true)}>Use saved secret</Button>
                    <Button size="sm" variant="ghost" disabled={Boolean(busy) && busy !== 'forget'} loading={busy === 'forget'} onClick={() => void forget()}>Forget saved</Button>
                </>}
            </div>}
        </div>
        <footer className="secret-card__foot">
            <span className="secret-card__assure" id={`${id}-assure`}><ShieldCheck size={13} aria-hidden /><span>{assurance(host, localPlatform)}</span></span>
            <div className="secret-card__actions">
                <Button variant="ghost" disabled={Boolean(busy)} loading={busy === 'deny'} onClick={() => void resolve('Deny', 'deny')}>Cancel</Button>
                <Button variant="primary" disabled={!hasValue || (Boolean(busy) && busy !== 'submit')} loading={busy === 'submit'}
                    disabledReason={!hasValue && !busy ? 'Enter the secret first' : undefined} onClick={() => void submit()}>{error ? 'Retry' : 'Submit'}</Button>
            </div>
        </footer>
    </section>
}
