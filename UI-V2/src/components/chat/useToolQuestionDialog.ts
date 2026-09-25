import { useEffect, useRef } from 'react'

/** Keep keyboard focus inside the topmost tool/question overlay and return it on dismissal. */
export function useToolQuestionDialog(onClose: () => void)
{
    const dialogRef = useRef<HTMLElement>(null)
    const closeRef = useRef(onClose)
    closeRef.current = onClose
    useEffect(() => {
        const dialog = dialogRef.current
        const trigger = document.activeElement instanceof HTMLElement ? document.activeElement : null
        dialog?.focus()
        const onKey = (event: KeyboardEvent) => {
            const modals = document.querySelectorAll('[role="dialog"][aria-modal="true"], [role="alertdialog"][aria-modal="true"]')
            if (modals[modals.length - 1] !== dialog || event.defaultPrevented || event.isComposing) return
            if (event.key === 'Escape') {
                event.preventDefault()
                event.stopImmediatePropagation()
                closeRef.current()
            } else if (event.key === 'Tab' && dialog) {
                const controls = Array.from(dialog.querySelectorAll<HTMLElement>('button:not(:disabled), input:not(:disabled), textarea:not(:disabled), a[href], [tabindex="0"]'))
                    .filter(element => !element.closest('[hidden]') && element.tabIndex >= 0)
                const first = controls[0]
                const last = controls[controls.length - 1]
                if (!first) { event.preventDefault(); dialog.focus() }
                else if (event.shiftKey && (document.activeElement === first || document.activeElement === dialog)) {
                    event.preventDefault(); last.focus()
                } else if (!event.shiftKey && (document.activeElement === last || document.activeElement === dialog)) {
                    event.preventDefault(); first.focus()
                }
            }
        }
        const keepFocus = (event: FocusEvent) => {
            const modals = document.querySelectorAll('[role="dialog"][aria-modal="true"], [role="alertdialog"][aria-modal="true"]')
            if (modals[modals.length - 1] === dialog && !dialog?.contains(event.target as Node)) dialog?.focus()
        }
        window.addEventListener('keydown', onKey, true)
        document.addEventListener('focusin', keepFocus)
        return () => {
            window.removeEventListener('keydown', onKey, true)
            document.removeEventListener('focusin', keepFocus)
            if (trigger?.isConnected) trigger.focus()
        }
    }, [])
    return dialogRef
}
