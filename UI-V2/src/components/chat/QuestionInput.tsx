import { useEffect, useId, useLayoutEffect, useRef, useState, type KeyboardEvent, type MutableRefObject, type ReactNode } from 'react'
import { Check, ChevronDown, ChevronLeft, ChevronRight, ChevronUp, MessageSquare } from 'lucide-react'
import { Button, IconButton, usePresence } from '../ui'
import type { AcpPendingUserInput, AcpUserInputAnswers } from '../../store/useAppStore'
import { SecretRequestCard } from './SecretRequestCard'
import './QuestionInput.css'

type Props = {
    input: AcpPendingUserInput
    onResolve: (requestId: string, answers: AcpUserInputAnswers) => Promise<boolean>
    /** The chat the request belongs to; a secret request submits its value through it. */
    sessionId?: string
    autoOpen?: boolean
    waitIsStale?: boolean
    waitStaleReason?: string
    waitSeconds?: number
    onCancelTurn?: () => void
    onStopRuntime?: () => void
    /** `float`: own card above the composer with its own Submit. `composer`: replaces the composer body; the composer send button submits. */
    variant?: 'float' | 'composer'
    /** Composer variant: the composer form calls this to submit the answers. */
    submitRef?: MutableRefObject<(() => void) | null>
    /** Composer variant: `ready` lets the composer send button submit; `busy` keeps it shown but disabled while answers are in flight. */
    onSubmitStateChange?: (state: QuestionSubmitState) => void
    /** Called once the provider accepts the answers, even if this card has unmounted meanwhile. */
    onAnswered?: () => void
    /** Owner-held answer state, so moving the question between the composer and the floating card keeps drafts, step and an in-flight submit. */
    memory?: QuestionMemory
}
export type QuestionSubmitState = 'incomplete' | 'ready' | 'busy'
type Draft = { choice: number | 'other' | null; selections?: Array<number | 'other'>; text: string }
export type QuestionMemory = { drafts: Record<string, Draft>; step: number; accepted: boolean; inflight?: Promise<boolean> }
export const createQuestionMemory = (): QuestionMemory => ({ drafts: {}, step: 0, accepted: false })
type Question = AcpPendingUserInput['questions'][number]

function answerFor(question: Question, draft?: Draft)
{
    if (!draft) return []
    if (question.isMultiple)
    {
        const selections = draft.selections ?? []
        if (selections.includes('other') && !draft.text.trim()) return []
        const answers = question.options.flatMap((option, index) => selections.includes(index) ? [option.label] : [])
        if (selections.includes('other') || question.options.length === 0) answers.push(draft.text.trim())
        return answers.filter(answer => answer.trim())
    }
    const answer = (typeof draft.choice === 'number' ? question.options[draft.choice]?.label ?? '' : draft.text).trim()
    return answer ? [answer] : []
}

/** Keep question drafts scoped to this chat and request while the panel is collapsed. */
export function UserInputInlineCard(props: Props)
{
    // A secret request is never a free-text question, even when its details failed validation.
    if (props.input.secretRequest || props.input.questions[0]?.id === 'secretRequest')
        return <SecretRequestCard key={props.input.requestId} input={props.input} sessionId={props.sessionId} onResolve={props.onResolve} />
    return <QuestionRequest key={`${props.input.requestId}:${props.input.itemId}`} {...props} />
}

/** Grid-rows collapse: the panel stays mounted for its exit, then unmounts (reduced motion and Motion Off unmount at once). */
export function QuestionCollapse({ open, children }: { open: boolean; children: ReactNode }) {
    const { mounted, closing } = usePresence(open)
    const lastChildren = useRef(children)
    if (open) lastChildren.current = children
    if (!mounted) return null
    return <div className="qr-collapse" data-state={closing ? 'closed' : 'open'}>
        <div className="qr-collapse__inner" {...(closing ? { inert: '' } : {})}>{open ? children : lastChildren.current}</div>
    </div>
}

const choiceInputs = (root: Element) => Array.from(root.querySelectorAll<HTMLInputElement>('input[type="radio"], input[type="checkbox"]'))

function QuestionRequest({ input, onResolve, sessionId: _sessionId, autoOpen = true, variant = 'float', submitRef, onSubmitStateChange, onAnswered, memory, ...wait }: Props)
{
    const [open, setOpen] = useState(autoOpen)
    const [drafts, setDrafts] = useState<Record<string, Draft>>(() => memory?.drafts ?? {})
    const [step, setStep] = useState(() => memory?.step ?? 0)
    const [submitting, setSubmitting] = useState(() => Boolean(memory?.inflight))
    const [accepted, setAccepted] = useState(() => memory?.accepted ?? false)
    const [error, setError] = useState('')
    const [restoreFocus, setRestoreFocus] = useState(false)
    const submittingRef = useRef(Boolean(memory?.inflight))
    useEffect(() => { if (memory) { memory.drafts = drafts; memory.step = step } }, [memory, drafts, step])
    const triggerRef = useRef<HTMLButtonElement>(null)
    const panelRef = useRef<HTMLElement>(null)
    const mountedRef = useRef(false)
    useEffect(() => { mountedRef.current = true; return () => { mountedRef.current = false } }, [])
    // The collapse and expand controls are different buttons, so focus moves after the swap renders.
    useLayoutEffect(() => { if (restoreFocus) { triggerRef.current?.focus(); setRestoreFocus(false) } }, [restoreFocus])
    // Replacing the composer drops focus from its textarea; hand it to the first answer instead of the page body.
    useEffect(() => {
        if (variant !== 'composer' || (document.activeElement && document.activeElement !== document.body)) return
        panelRef.current?.querySelector<HTMLInputElement>('fieldset input')?.focus()
    }, [variant])
    const pending = !input.status || input.status === 'pending'
    const busy = submitting || !pending
    const current = Math.min(step, Math.max(0, input.questions.length - 1))
    const question = input.questions[current]
    const close = (focusTrigger = true) => { setOpen(false); setRestoreFocus(focusTrigger) }
    const complete = input.questions.length > 0 && input.questions.every(item => answerFor(item, drafts[item.id]).length > 0)
    const answered = question ? answerFor(question, drafts[question.id]).length > 0 : false
    const last = current >= input.questions.length - 1
    // Every card instance for this request awaits the same submit, so a remount mid-submit stays busy and cannot send twice.
    const settle = async (request: Promise<boolean>) => {
        let resolved = false
        try {
            resolved = await request
            if (resolved) { if (memory) { memory.accepted = true; memory.drafts = {} } onAnswered?.() }
            if (!mountedRef.current) return
            if (resolved) { setAccepted(true); setDrafts({}); close(false) }
            else setError('The provider did not accept the answers. Try again.')
        } catch {
            if (!mountedRef.current) return
            setError('Could not submit the answers. Try again.')
        } finally {
            if (memory?.inflight === request) memory.inflight = undefined
            if (!resolved && mountedRef.current) { submittingRef.current = false; setSubmitting(false) }
        }
    }
    const submit = async () => {
        if (!pending || !complete || submittingRef.current || memory?.inflight) return
        submittingRef.current = true
        setSubmitting(true)
        setError('')
        const answers: AcpUserInputAnswers = Object.fromEntries(input.questions.map(item => [item.id, answerFor(item, drafts[item.id])]))
        const request = (async () => onResolve(input.requestId, answers))()
        if (memory) memory.inflight = request
        await settle(request)
    }
    // Adopt a submit started by the instance this one replaced. A layout effect runs before the response can settle, and
    // a submit that already failed in between leaves nothing in flight, so the busy state seeded at render is dropped.
    // eslint-disable-next-line react-hooks/exhaustive-deps
    useLayoutEffect(() => {
        if (memory?.inflight) void settle(memory.inflight)
        else if (submittingRef.current) { submittingRef.current = false; setSubmitting(false) }
    }, [])
    const submitState: QuestionSubmitState = busy ? 'busy' : pending && complete && !accepted ? 'ready' : 'incomplete'
    useEffect(() => { onSubmitStateChange?.(submitState) }, [submitState, onSubmitStateChange])
    useEffect(() => {
        if (!submitRef) return
        submitRef.current = () => void submit()
        return () => { submitRef.current = null }
    })
    // Enter advances to the next question, or submits on the last one; it never submits the surrounding composer form.
    const onKeyDown = (event: KeyboardEvent) => {
        if (event.nativeEvent.isComposing || event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return
        if (event.key !== 'Enter' || !(event.target instanceof HTMLInputElement)) return
        event.preventDefault()
        event.stopPropagation()
        if (!last) { if (answered && !busy) setStep(current + 1) }
        else void submit()
    }
    const status = busy ? 'Submitting answers…' : question?.isMultiple ? `${drafts[question.id]?.selections?.length ?? 0} selected`
        : input.questions.length > 1 ? `Question ${current + 1} of ${input.questions.length}` : ''
    const steps = input.questions.length > 1 && <>
        <Button type="button" variant="ghost" size="sm" leadingIcon={<ChevronLeft size={14} aria-hidden />} disabled={current === 0 || busy} onClick={() => setStep(current - 1)}>Back</Button>
        {!last && <Button type="button" variant={variant === 'composer' ? 'ghost' : 'primary'} size="sm" disabled={busy || !answered} onClick={() => setStep(current + 1)} trailingIcon={<ChevronRight size={14} aria-hidden />}>Next</Button>}
    </>
    const body = <QuestionPanel input={input} drafts={drafts} step={current}
        onDraft={(id, draft) => setDrafts(currentDrafts => ({ ...currentDrafts, [id]: draft }))} busy={busy} error={error} {...wait} />

    if (variant === 'composer') return <section ref={panelRef} className="qr-panel" data-variant="composer" aria-label="Answer questions" data-testid="user-input-card" onKeyDown={onKeyDown}>
        <div className="qr-meta">
            <span className="qr-kicker">{question?.header || 'Question'}</span>
            <span className="qr-status" role="status">{status}</span>
            {steps && <span className="qr-actions">{steps}</span>}
        </div>
        {body}
    </section>

    const expanded = open && !accepted
    return <section ref={panelRef} className="qr-panel" data-variant="float" data-open={expanded} aria-label="Answer questions" data-testid="user-input-card" onKeyDown={onKeyDown}>
        <header className="qr-header">
            {accepted ? <Check size={14} aria-hidden className="qr-header__done" /> : <MessageSquare size={14} aria-hidden />}
            <h2>{accepted ? 'Answers submitted' : question?.header || 'Question'}</h2>
            {!accepted && (expanded
                ? <IconButton ref={triggerRef} size="sm" icon={<ChevronDown size={14} aria-hidden />} label="Collapse questions" tooltip="Hide until you are ready to answer" aria-expanded onClick={() => close()} disabled={!pending} />
                : <Button ref={triggerRef} type="button" variant="ghost" size="sm" aria-expanded={false} onClick={() => setOpen(true)} disabled={!pending} trailingIcon={<ChevronUp size={14} aria-hidden />}>Answer questions</Button>)}
        </header>
        <QuestionCollapse open={expanded}>
            {body}
            <footer className="qr-footer">
                <span className="qr-status" role="status">{status}</span>
                <div className="qr-actions">
                    {steps}
                    {last && <Button type="button" variant="primary" size="sm" disabled={busy || !complete} onClick={() => void submit()}>{busy ? 'Submitting…' : error ? 'Retry submission' : 'Submit answers'}</Button>}
                </div>
            </footer>
        </QuestionCollapse>
    </section>
}

function QuestionPanel({ input, drafts, step, onDraft, busy, error, waitIsStale, waitStaleReason, waitSeconds, onCancelTurn, onStopRuntime }: Pick<Props, 'input' | 'waitIsStale' | 'waitStaleReason' | 'waitSeconds' | 'onCancelTurn' | 'onStopRuntime'> & {
    drafts: Record<string, Draft>; step: number; onDraft: (id: string, draft: Draft) => void; busy: boolean; error: string
})
{
    const headingRef = useRef<HTMLLegendElement>(null)
    const textRef = useRef<HTMLInputElement>(null)
    const id = useId()
    const question = input.questions[step]
    const draft = drafts[question?.id] ?? { choice: null, text: '' }
    const choose = (choice: Draft['choice']) => {
        if (question.isMultiple && choice !== null)
        {
            const selections = draft.selections ?? []
            const selected = selections.includes(choice)
            onDraft(question.id, { ...draft, selections: selected ? selections.filter(item => item !== choice) : [...selections, choice] })
            if (choice === 'other' && !selected) textRef.current?.focus()
        }
        else
        {
            onDraft(question.id, { ...draft, choice })
            if (choice === 'other') textRef.current?.focus()
        }
    }
    const previousStep = useRef(step)
    useEffect(() => {
        if (previousStep.current !== step) headingRef.current?.focus()
        previousStep.current = step
    }, [step])
    const otherSelected = question?.isMultiple ? draft.selections?.includes('other') ?? false : draft.choice === 'other'
    const textInput = question && <input ref={textRef} id={`${id}-answer`} className="qr-text-input"
        aria-label={question.options.length ? 'Write your own answer' : question.question || question.header || question.id}
        placeholder={question.isSecret ? 'Type the secret answer' : question.options.length ? 'Type your own answer' : 'Type your answer'}
        type={question.isSecret ? 'password' : 'text'} autoComplete="off" spellCheck={!question.isSecret} value={draft.text}
        onFocus={() => { if (question.options.length && !otherSelected) choose('other') }}
        onChange={event => onDraft(question.id, { ...draft, choice: 'other', selections: question.isMultiple ? [...(draft.selections ?? []).filter(item => item !== 'other'), 'other'] : draft.selections, text: event.target.value })} />
    return <div className="qr-body" aria-busy={busy}>
        {waitIsStale && <div data-testid="stale-wait-warning" className="qr-warning">
            <p>No response for {Math.max(120, waitSeconds ?? 0)}s.{waitStaleReason && <> {waitStaleReason}</>}</p>
            {onCancelTurn && <Button size="sm" disabled={busy} onClick={onCancelTurn}>Cancel turn</Button>}
            {onStopRuntime && <Button size="sm" disabled={busy} onClick={onStopRuntime}>Stop runtime</Button>}
        </div>}
        {question ? <fieldset key={question.id} className={`qr-question${question.isMultiple ? ' qr-multiple' : ''}`} disabled={busy} onKeyDown={event => {
            if (event.altKey || event.ctrlKey || event.metaKey || event.nativeEvent.isComposing) return
            const target = event.target
            const choices = choiceInputs(event.currentTarget)
            const typing = target instanceof HTMLInputElement && !['radio', 'checkbox'].includes(target.type)
            if ((event.key === 'ArrowDown' || event.key === 'ArrowUp') && target instanceof HTMLInputElement && choices.length)
            {
                // Rows move like a radio group (wrapping); single choice selects as it moves, multiple choice only moves focus.
                const from = typing ? choices.findIndex(item => item.dataset.other !== undefined) : choices.indexOf(target)
                if (from < 0) return
                event.preventDefault()
                const next = choices[(from + (event.key === 'ArrowDown' ? 1 : -1) + choices.length) % choices.length]
                next.focus()
                if (next.type === 'radio' && !next.checked) next.click()
                return
            }
            if (typing || !/^[1-9]$/.test(event.key)) return
            const option = choices[Number(event.key) - 1]
            if (option) { event.preventDefault(); option.focus(); option.click() }
        }}>
            <legend ref={headingRef} tabIndex={-1}>{question.question || question.header || 'Your answer'}</legend>
            {question.isMultiple && question.options.length > 0 && <p className="qr-multiple-hint">Select one or more.</p>}
            {question.options.length > 0 && <div className="qr-options">
                {question.options.map((option, index) => <label className="qr-option" key={`${index}-${option.label}`}>
                    <input type={question.isMultiple ? 'checkbox' : 'radio'} name={`${id}-${question.id}`} checked={question.isMultiple ? draft.selections?.includes(index) ?? false : draft.choice === index} onChange={() => choose(index)} />
                    <span className="qr-number" aria-hidden>{index + 1}</span>
                    <span className="qr-option-copy"><span className="qr-option-label">{option.label}</span>{option.description && <small className="qr-option-description">{option.description}</small>}</span>
                    <Check className="qr-selected" size={14} aria-hidden />
                </label>)}
                {question.isOther && <label className="qr-option qr-other">
                    <input type={question.isMultiple ? 'checkbox' : 'radio'} aria-label="Other" data-other="" name={`${id}-${question.id}`} checked={otherSelected} onChange={() => choose('other')} />
                    <span className="qr-number" aria-hidden>{question.options.length + 1}</span>
                    <span className="qr-option-copy qr-text-answer">{textInput}</span>
                    <Check className="qr-selected" size={14} aria-hidden />
                </label>}
            </div>}
            {question.options.length === 0 && <div className="qr-text-answer qr-text-answer--solo">{textInput}</div>}
        </fieldset> : <p>No questions are available.</p>}
        {error && <p role="alert" className="qr-error">{error}</p>}
    </div>
}
