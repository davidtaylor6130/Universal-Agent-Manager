import { useEffect, useId, useRef, useState } from 'react'
import { Check, ChevronLeft, ChevronRight, MessageSquare, ChevronUp, ChevronDown } from 'lucide-react'
import type { AcpPendingUserInput, AcpUserInputAnswers } from '../../store/useAppStore'
import './QuestionInput.css'

type Props = {
    input: AcpPendingUserInput
    onResolve: (requestId: string, answers: AcpUserInputAnswers) => Promise<boolean>
    autoOpen?: boolean
    waitIsStale?: boolean
    waitStaleReason?: string
    waitSeconds?: number
    onCancelTurn?: () => void
    onStopRuntime?: () => void
}
type Draft = { choice: number | 'other' | null; selections?: Array<number | 'other'>; text: string }
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
    return <QuestionRequest key={`${props.input.requestId}:${props.input.itemId}`} {...props} />
}

function QuestionRequest({ input, onResolve, autoOpen = true, ...wait }: Props)
{
    const [open, setOpen] = useState(autoOpen)
    const [drafts, setDrafts] = useState<Record<string, Draft>>({})
    const [step, setStep] = useState(0)
    const [submitting, setSubmitting] = useState(false)
    const [accepted, setAccepted] = useState(false)
    const [error, setError] = useState('')
    const submittingRef = useRef(false)
    const triggerRef = useRef<HTMLButtonElement>(null)
    const mountedRef = useRef(false)
    useEffect(() => { mountedRef.current = true; return () => { mountedRef.current = false } }, [])
    const pending = !input.status || input.status === 'pending'
    const close = (restoreFocus = true) => { setOpen(false); if (restoreFocus) triggerRef.current?.focus() }
    const complete = input.questions.length > 0 && input.questions.every(question => answerFor(question, drafts[question.id]).length > 0)
    const submit = async () => {
        if (!pending || !complete || submittingRef.current) return
        submittingRef.current = true
        setSubmitting(true)
        setError('')
        const answers: AcpUserInputAnswers = Object.fromEntries(input.questions.map(question => [question.id, answerFor(question, drafts[question.id])]))
        let resolved = false
        try {
            resolved = await onResolve(input.requestId, answers)
            if (!mountedRef.current) return
            if (resolved) { setAccepted(true); setDrafts({}); close(false) }
            else setError('The provider did not accept the answers. Try again.')
        } catch {
            if (!mountedRef.current) return
            setError('Could not submit the answers. Try again.')
        } finally {
            if (!resolved && mountedRef.current) { submittingRef.current = false; setSubmitting(false) }
        }
    }
    return <section className="qr-panel" aria-label="Answer questions" data-testid="user-input-card" onKeyDown={event => {
        if (event.nativeEvent.isComposing || event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return
        if (event.key === 'Enter' && event.target instanceof HTMLInputElement && !['radio', 'checkbox'].includes(event.target.type))
        {
            event.preventDefault()
            event.stopPropagation()
            void submit()
        }
    }}>
        <header className="qr-header">
            <MessageSquare size={14} aria-hidden />
            <h2>{accepted ? 'Answers submitted' : input.questions[step]?.header || 'Answer questions'}</h2>
            <span className="qr-count">{Math.min(step + 1, input.questions.length)} of {input.questions.length}</span>
            <button ref={triggerRef} type="button" aria-expanded={open && !accepted} onClick={() => { if (open) close(); else setOpen(true) }} disabled={accepted || !pending}>
                {accepted ? 'Answers submitted' : open ? <><ChevronUp size={14} aria-hidden />Collapse questions</> : <><ChevronDown size={14} aria-hidden />Answer questions</>}
            </button>
        </header>
        {open && !accepted && <QuestionPanel input={input} drafts={drafts} step={Math.min(step, Math.max(0, input.questions.length - 1))}
            onDraft={(id, draft) => setDrafts(current => ({ ...current, [id]: draft }))} onStep={setStep}
            onSubmit={() => void submit()} busy={submitting || !pending} complete={complete} error={error} {...wait} />}
    </section>
}

function QuestionPanel({ input, drafts, step, onDraft, onStep, onSubmit, busy, complete, error,
    waitIsStale, waitStaleReason, waitSeconds, onCancelTurn, onStopRuntime }: Omit<Props, 'onResolve'> & {
    drafts: Record<string, Draft>; step: number; onDraft: (id: string, draft: Draft) => void
    onStep: (step: number) => void; onSubmit: () => void
    busy: boolean; complete: boolean; error: string
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
    return <>
            {input.questions.length > 1 && <nav className="qr-progress" aria-label="Question progress">
                {input.questions.map((item, index) => <button key={item.id} type="button" disabled={busy} aria-current={index === step ? 'step' : undefined} onClick={() => onStep(index)}>
                    {answerFor(item, drafts[item.id]).length > 0 ? <Check size={12} aria-hidden /> : <span>{index + 1}</span>}
                    {item.header || `Question ${index + 1}`}
                    <span className="qr-sr-only">{answerFor(item, drafts[item.id]).length > 0 ? ', answered' : ', unanswered'}</span>
                </button>)}
            </nav>}
            <div className="qr-body" aria-busy={busy}>
                {waitIsStale && <div data-testid="stale-wait-warning" className="qr-warning">
                    <p>This input request has had no runtime activity for {Math.max(120, waitSeconds ?? 0)}s.</p>
                    {waitStaleReason && <p>{waitStaleReason}</p>}
                    {onCancelTurn && <button type="button" disabled={busy} onClick={onCancelTurn}>Cancel turn</button>}
                    {onStopRuntime && <button type="button" disabled={busy} onClick={onStopRuntime}>Stop runtime</button>}
                </div>}
                {question ? <fieldset key={question.id} className={`qr-question${question.isMultiple ? ' qr-multiple' : ''}`} disabled={busy} onKeyDown={event => {
                    if (event.altKey || event.ctrlKey || event.metaKey || event.nativeEvent.isComposing) return
                    if (event.target instanceof HTMLInputElement && !['radio', 'checkbox'].includes(event.target.type)) return
                    if (!/^[1-9]$/.test(event.key)) return
                    const option = event.currentTarget.querySelectorAll<HTMLInputElement>('input[type="radio"], input[type="checkbox"]')[Number(event.key) - 1]
                    if (option) { event.preventDefault(); option.focus(); option.click() }
                }}>
                    <legend ref={headingRef} tabIndex={-1}>{question.question || question.header || 'Your answer'}</legend>
                    {question.isMultiple && question.options.length > 0 && <p className="qr-multiple-hint">Select one or more.</p>}
                    {question.options.length > 0 && <div className="qr-options">
                        {question.options.map((option, index) => <label className="qr-option" key={`${index}-${option.label}`}>
                            <input type={question.isMultiple ? "checkbox" : "radio"} name={`${id}-${question.id}`} checked={question.isMultiple ? draft.selections?.includes(index) ?? false : draft.choice === index} onChange={() => choose(index)} />
                            <span className="qr-number" aria-hidden>{index + 1}</span>
                            <span className="qr-option-copy"><span>{option.label}</span>{option.description && <small className="qr-sr-only">{option.description}</small>}</span>
                            <Check className="qr-selected" size={16} aria-hidden />
                        </label>)}
                        {question.isOther && <label className="qr-option qr-other">
                            <input type={question.isMultiple ? "checkbox" : "radio"} name={`${id}-${question.id}`} checked={question.isMultiple ? draft.selections?.includes('other') ?? false : draft.choice === 'other'} onChange={() => choose('other')} />
                            <span className="qr-number" aria-hidden>{question.options.length + 1}</span><span className="qr-option-copy">Other</span><Check className="qr-selected" size={16} aria-hidden />
                        </label>}
                    </div>}
                    {(question.isOther || question.options.length === 0) && <div className="qr-text-answer">
                        <label htmlFor={`${id}-answer`}>{question.isSecret ? 'Secret answer' : question.options.length ? 'Write your own answer' : 'Your answer'}</label>
                        <input ref={textRef} id={`${id}-answer`} aria-label={question.question || question.header || question.id}
                            type={question.isSecret ? 'password' : 'text'} autoComplete="off" spellCheck={!question.isSecret} value={draft.text}
                            onFocus={() => { if (question.options.length && (question.isMultiple ? !draft.selections?.includes('other') : draft.choice !== 'other')) choose('other') }}
                            onChange={event => onDraft(question.id, { ...draft, choice: 'other', selections: question.isMultiple ? [...(draft.selections ?? []).filter(item => item !== 'other'), 'other'] : draft.selections, text: event.target.value })} />
                    </div>}
                </fieldset> : <p>No questions are available.</p>}
                {error && <p role="alert" className="qr-error">{error}</p>}
            </div>
            <footer className="qr-footer">
                <span className="qr-status" role="status">{busy ? 'Submitting answers…' : question?.isMultiple ? `${draft.selections?.length ?? 0} selected` : `Question ${question ? step + 1 : 0} of ${input.questions.length}`}</span>
                <div className="qr-actions">
                    {input.questions.length > 1 && <button type="button" disabled={step === 0 || busy} onClick={() => onStep(step - 1)}><ChevronLeft size={14} />Back</button>}
                    {step < input.questions.length - 1 ? <button type="button" className="qr-primary" disabled={busy || answerFor(question, draft).length === 0} onClick={() => onStep(step + 1)}>Next<ChevronRight size={14} /></button>
                        : <button type="button" className="qr-primary" disabled={busy || !complete} onClick={onSubmit}>{busy ? 'Submitting…' : error ? 'Retry submission' : 'Submit answers'}</button>}
                </div>
            </footer>
    </>
}
