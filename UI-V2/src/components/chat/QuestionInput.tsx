import { useEffect, useId, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { Check, ChevronLeft, ChevronRight, MessageSquare, X } from 'lucide-react'
import type { AcpPendingUserInput, AcpUserInputAnswers } from '../../store/useAppStore'
import { useToolQuestionDialog } from './useToolQuestionDialog'
import './QuestionInput.css'

type Props = {
    input: AcpPendingUserInput
    onResolve: (requestId: string, answers: AcpUserInputAnswers) => Promise<boolean>
    waitIsStale?: boolean
    waitStaleReason?: string
    waitSeconds?: number
    onCancelTurn?: () => void
    onStopRuntime?: () => void
}
type Draft = { choice: number | 'other' | null; text: string }
type Question = AcpPendingUserInput['questions'][number]

function answerFor(question: Question, draft?: Draft)
{
    if (!draft) return ''
    return (typeof draft.choice === 'number' ? question.options[draft.choice]?.label ?? '' : draft.text).trim()
}

/** Key the draft owner by request, while closing only unmounts the dialog. */
export function UserInputInlineCard(props: Props)
{
    return <QuestionRequest key={`${props.input.requestId}:${props.input.itemId}`} {...props} />
}

function QuestionRequest({ input, onResolve, ...wait }: Props)
{
    const [open, setOpen] = useState(false)
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
    useEffect(() => {
        // Auto-open once when no other modal is taking input. Otherwise leave the trigger available.
        if (pending && input.questions.length && !document.querySelector('[aria-modal="true"]')) { triggerRef.current?.focus(); setOpen(true) }
        // Request identity remounts this owner; provider refreshes must not reopen a dismissed dialog.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [])
    const close = () => { setOpen(false); triggerRef.current?.focus() }
    const complete = input.questions.length > 0 && input.questions.every(question => answerFor(question, drafts[question.id]))
    const submit = async () => {
        if (!pending || !complete || submittingRef.current) return
        submittingRef.current = true
        setSubmitting(true)
        setError('')
        const answers: AcpUserInputAnswers = Object.fromEntries(input.questions.map(question => [question.id, [answerFor(question, drafts[question.id])]]))
        let resolved = false
        try {
            resolved = await onResolve(input.requestId, answers)
            if (!mountedRef.current) return
            if (resolved) { setAccepted(true); setDrafts({}); close() }
            else setError('The provider did not accept the answers. Try again.')
        } catch {
            if (!mountedRef.current) return
            setError('Could not submit the answers. Try again.')
        } finally {
            if (!resolved && mountedRef.current) { submittingRef.current = false; setSubmitting(false) }
        }
    }
    return <div className="question-trigger" data-testid="user-input-card">
        <button ref={triggerRef} type="button" aria-haspopup="dialog" onClick={() => setOpen(true)} disabled={accepted || !pending}>
            <MessageSquare size={14} aria-hidden />{accepted ? 'Answers submitted' : 'Answer questions'}
        </button>
        {open && <QuestionDialog input={input} drafts={drafts} step={Math.min(step, Math.max(0, input.questions.length - 1))}
            onDraft={(id, draft) => setDrafts(current => ({ ...current, [id]: draft }))} onStep={setStep}
            onClose={close} onSubmit={() => void submit()} busy={submitting || !pending} complete={complete} error={error} {...wait} />}
    </div>
}

function QuestionDialog({ input, drafts, step, onDraft, onStep, onClose, onSubmit, busy, complete, error,
    waitIsStale, waitStaleReason, waitSeconds, onCancelTurn, onStopRuntime }: Omit<Props, 'onResolve'> & {
    drafts: Record<string, Draft>; step: number; onDraft: (id: string, draft: Draft) => void
    onStep: (step: number) => void; onClose: () => void; onSubmit: () => void
    busy: boolean; complete: boolean; error: string
})
{
    const dialogRef = useToolQuestionDialog(onClose)
    const headingRef = useRef<HTMLLegendElement>(null)
    const textRef = useRef<HTMLInputElement>(null)
    const id = useId()
    const question = input.questions[step]
    const draft = drafts[question?.id] ?? { choice: null, text: '' }
    const choose = (choice: Draft['choice']) => {
        onDraft(question.id, { ...draft, choice })
        if (choice === 'other') textRef.current?.focus()
    }
    useEffect(() => { headingRef.current?.focus() }, [step])
    return createPortal(<div className="question-backdrop" onMouseDown={event => { if (event.target === event.currentTarget) onClose() }}>
        <section ref={dialogRef} role="dialog" aria-modal="true" aria-labelledby={`${id}-title`} tabIndex={-1} className="qr-dialog">
            <header className="qr-header">
                <MessageSquare size={17} aria-hidden />
                <h2 id={`${id}-title`}>Answer questions</h2>
                <button className="qr-close" type="button" aria-label="Close questions" title="Close questions (Esc)" onClick={onClose}><X size={17} /></button>
            </header>
            <nav className="qr-progress" aria-label="Question progress">
                {input.questions.map((item, index) => <button key={item.id} type="button" disabled={busy} aria-current={index === step ? 'step' : undefined} onClick={() => onStep(index)}>
                    {answerFor(item, drafts[item.id]) ? <Check size={12} aria-hidden /> : <span>{index + 1}</span>}
                    {item.header || `Question ${index + 1}`}
                    <span className="qr-sr-only">{answerFor(item, drafts[item.id]) ? ', answered' : ', unanswered'}</span>
                </button>)}
            </nav>
            <div className="qr-body" aria-busy={busy}>
                {waitIsStale && <div data-testid="stale-wait-warning" className="qr-warning">
                    <p>This input request has had no runtime activity for {Math.max(120, waitSeconds ?? 0)}s.</p>
                    {waitStaleReason && <p>{waitStaleReason}</p>}
                    {onCancelTurn && <button type="button" disabled={busy} onClick={onCancelTurn}>Cancel turn</button>}
                    {onStopRuntime && <button type="button" disabled={busy} onClick={onStopRuntime}>Stop runtime</button>}
                </div>}
                {question ? <fieldset key={question.id} className="qr-question" disabled={busy} onKeyDown={event => {
                    if (event.altKey || event.ctrlKey || event.metaKey || event.nativeEvent.isComposing) return
                    if (event.target instanceof HTMLInputElement && event.target.type !== 'radio') return
                    if (!/^[1-9]$/.test(event.key)) return
                    const radio = event.currentTarget.querySelectorAll<HTMLInputElement>('input[type="radio"]')[Number(event.key) - 1]
                    if (radio) { event.preventDefault(); radio.focus(); radio.click() }
                }}>
                    <legend ref={headingRef} tabIndex={-1}>{question.question || question.header || 'Your answer'}</legend>
                    {question.options.length > 0 && <div className="qr-options">
                        {question.options.map((option, index) => <label className="qr-option" key={`${index}-${option.label}`}>
                            <input type="radio" name={`${id}-${question.id}`} checked={draft.choice === index} onChange={() => choose(index)} />
                            <span className="qr-number" aria-hidden>{index + 1}</span>
                            <span className="qr-option-copy"><span>{option.label}</span>{option.description && <small>{option.description}</small>}</span>
                            <Check className="qr-selected" size={16} aria-hidden />
                        </label>)}
                        {question.isOther && <label className="qr-option qr-other">
                            <input type="radio" name={`${id}-${question.id}`} checked={draft.choice === 'other'} onChange={() => choose('other')} />
                            <span className="qr-number" aria-hidden>{question.options.length + 1}</span><span className="qr-option-copy">Other</span><Check className="qr-selected" size={16} aria-hidden />
                        </label>}
                    </div>}
                    {(question.isOther || question.options.length === 0) && <div className="qr-text-answer">
                        <label htmlFor={`${id}-answer`}>{question.isSecret ? 'Secret answer' : question.options.length ? 'Write your own answer' : 'Your answer'}</label>
                        <input ref={textRef} id={`${id}-answer`} aria-label={question.question || question.header || question.id}
                            type={question.isSecret ? 'password' : 'text'} autoComplete="off" spellCheck={!question.isSecret} value={draft.text}
                            onFocus={() => { if (question.options.length && draft.choice !== 'other') choose('other') }}
                            onChange={event => onDraft(question.id, { choice: 'other', text: event.target.value })} />
                    </div>}
                </fieldset> : <p>No questions are available.</p>}
                {error && <p role="alert" className="qr-error">{error}</p>}
            </div>
            <footer className="qr-footer">
                <span className="qr-status" role="status">{busy ? 'Submitting answers…' : `Question ${question ? step + 1 : 0} of ${input.questions.length}`}</span>
                <div className="qr-actions">
                    <button type="button" disabled={step === 0 || busy} onClick={() => onStep(step - 1)}><ChevronLeft size={14} />Back</button>
                    {step < input.questions.length - 1 ? <button type="button" className="qr-primary" disabled={busy || !answerFor(question, draft)} onClick={() => onStep(step + 1)}>Next<ChevronRight size={14} /></button>
                        : <button type="button" className="qr-primary" disabled={busy || !complete} onClick={onSubmit}>{busy ? 'Submitting…' : error ? 'Retry submission' : 'Submit answers'}</button>}
                </div>
            </footer>
        </section>
    </div>, document.body)
}
