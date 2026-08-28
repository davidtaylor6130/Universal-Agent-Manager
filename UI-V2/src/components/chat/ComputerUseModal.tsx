import { useEffect, useRef, useState } from 'react'
import { MousePointer2, Pause, Play, Square, X } from 'lucide-react'
import type { ComputerUseActionResult, ComputerUseBackend, ComputerUseControlState, ComputerUseEffectiveBackend } from '../../types/session'
import { IconButton, Switch } from '../ui'

interface ComputerUseModalProps {
  active: boolean
  enabled: boolean
  disabled: boolean
  backend: ComputerUseBackend
  effectiveBackend: ComputerUseEffectiveBackend
  providerAvailable: boolean
  providerName: string
  modelLabel: string
  targetKind: 'window' | 'screen'
  targetTitle: string
  targetInputMode: 'background' | 'foreground'
  state: ComputerUseControlState
  onClose: () => void
  onSetActive: (active: boolean) => Promise<ComputerUseActionResult>
  onSetBackend: (backend: ComputerUseBackend) => Promise<ComputerUseActionResult>
  onSetControl: (state: 'running' | 'paused') => Promise<ComputerUseActionResult>
}

export function ComputerUseModal({
  active,
  enabled,
  disabled,
  backend,
  effectiveBackend,
  providerAvailable,
  providerName,
  modelLabel,
  targetKind,
  targetTitle,
  targetInputMode,
  state,
  onClose,
  onSetActive,
  onSetBackend,
  onSetControl,
}: ComputerUseModalProps) {
  const dialogRef = useRef<HTMLDivElement>(null)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')

  useEffect(() => {
    const previousFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null
    dialogRef.current?.focus()
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        onClose()
        return
      }
      if (event.key !== 'Tab') return

      const focusable = Array.from(dialogRef.current?.querySelectorAll<HTMLElement>(
        'button:not([disabled]), select:not([disabled]), input:not([disabled]), textarea:not([disabled]), a[href], [tabindex]:not([tabindex="-1"])'
      ) ?? []).filter((element) => element.getClientRects().length > 0)
      if (focusable.length === 0) {
        event.preventDefault()
        dialogRef.current?.focus()
        return
      }

      const first = focusable[0]
      const last = focusable[focusable.length - 1]
      if (event.shiftKey && (document.activeElement === first || !dialogRef.current?.contains(document.activeElement))) {
        event.preventDefault()
        last.focus()
      } else if (!event.shiftKey && (document.activeElement === last || !dialogRef.current?.contains(document.activeElement))) {
        event.preventDefault()
        first.focus()
      }
    }
    window.addEventListener('keydown', onKeyDown)
    return () => {
      window.removeEventListener('keydown', onKeyDown)
      previousFocus?.focus()
    }
    // The modal is mounted only while open, so focus should be captured and restored once.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  const run = async (action: () => Promise<ComputerUseActionResult>, message: string) => {
    setBusy(true)
    setError('')
    try {
      const result = await action()
      if (!result.ok) setError(result.error || message)
    } catch (cause) {
      setError(cause instanceof Error && cause.message ? cause.message : message)
    } finally {
      setBusy(false)
    }
  }

  const effectiveLabel = effectiveBackend === 'provider' ? 'Provider built-in' : 'UAM controlled'
  const automaticStatus = backend === 'auto'
    ? `Automatic currently uses ${effectiveLabel}.`
    : `Using ${effectiveLabel}.`

  return (
    <div
      className="fixed inset-0 z-[60] flex items-center justify-center animate-fade-in"
      style={{ background: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(4px)' }}
      onClick={(event) => { if (event.target === event.currentTarget) onClose() }}
    >
      <div
        ref={dialogRef}
        role="dialog"
        aria-modal="true"
        aria-label="Computer use"
        aria-labelledby="computer-use-title"
        aria-describedby="computer-use-description"
        tabIndex={-1}
        className="mx-4 flex max-h-[calc(100vh-2rem)] w-full max-w-md flex-col overflow-hidden rounded-xl shadow-2xl animate-slide-in"
        style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)' }}
      >
        <div className="flex items-center justify-between px-5 py-4" style={{ borderBottom: '1px solid var(--border)' }}>
          <div className="flex items-center gap-2">
            <MousePointer2 size={16} aria-hidden style={{ color: 'var(--accent)' }} />
            <div>
              <div id="computer-use-title" className="text-sm font-semibold" style={{ color: 'var(--text)' }}>Computer use</div>
              <div id="computer-use-description" className="text-[11px]" style={{ color: 'var(--text-3)' }}>Choose who controls apps for this chat.</div>
            </div>
          </div>
          <IconButton icon={<X size={16} />} label="Close computer use" onClick={onClose} />
        </div>

        <div className="grid gap-4 overflow-y-auto p-5 text-xs">
          {error && <div role="alert" className="rounded-md px-3 py-2" style={{ color: 'var(--red)', border: '1px solid color-mix(in srgb, var(--red) 35%, var(--border))', background: 'color-mix(in srgb, var(--red) 10%, transparent)' }}>{error}</div>}

          <section className="grid gap-2">
            <label htmlFor="computer-use-backend" className="font-medium" style={{ color: 'var(--text)' }}>Control method</label>
            <select
              id="computer-use-backend"
              aria-describedby="computer-use-backend-status"
              value={backend}
              disabled={active || busy || disabled}
              onChange={(event) => void run(
                () => onSetBackend(event.target.value as ComputerUseBackend),
                'The control method could not be changed.'
              )}
              className="w-full rounded-md px-3 py-2"
              style={{ border: '1px solid var(--border)', background: 'var(--surface-up)', color: 'var(--text)' }}
            >
              <option value="auto">Automatic (recommended)</option>
              <option value="provider" disabled={!providerAvailable}>Provider built-in</option>
              <option value="uam">UAM controlled</option>
            </select>
            <div
              id="computer-use-backend-status"
              role="status"
              className="rounded-md px-3 py-2"
              style={{ border: '1px solid var(--border)', background: 'color-mix(in srgb, var(--surface-up) 78%, transparent)', color: 'var(--text-2)' }}
            >
              {automaticStatus}
              {active && <span className="mt-1 block text-[11px]" style={{ color: 'var(--text-3)' }}>Turn computer use off before changing this method.</span>}
            </div>
            {providerAvailable ? (
              <span style={{ color: 'var(--text-3)' }}>
                Provider built-in is an unprobed preview for Codex structured sessions. UAM does not verify the installed Codex version supports it. Automatic uses UAM.
              </span>
            ) : (
              <span style={{ color: 'var(--text-3)' }}>
                {providerName} built-in computer use is unavailable in this structured session, so Automatic uses UAM.
              </span>
            )}
          </section>

          {effectiveBackend === 'provider' && (
            <div
              className="flex items-center justify-between gap-4 rounded-lg px-3 py-3"
              style={{
                border: `1px solid ${active ? 'color-mix(in srgb, var(--accent) 48%, var(--border))' : 'var(--border)'}`,
                background: active
                  ? 'linear-gradient(135deg, color-mix(in srgb, var(--accent) 18%, var(--surface-up)), color-mix(in srgb, var(--purple) 11%, var(--surface)))'
                  : 'color-mix(in srgb, var(--surface-up) 78%, transparent)',
              }}
            >
              <span>
                <span className="block font-medium" style={{ color: 'var(--text)' }}>Provider computer use</span>
                <span className="block text-[11px]" style={{ color: 'var(--text-3)' }}>The provider owns its approval workflow.</span>
              </span>
              <Switch
                hideLabel
                label="Provider computer use active"
                className="uam-computer-use-switch"
                checked={active}
                disabled={busy || (!active && disabled)}
                onChange={(event) => void run(() => onSetActive(event.target.checked), 'Computer use could not be changed.')}
              />
            </div>
          )}

          <div className="flex items-center justify-between gap-4">
            <span className="font-medium" style={{ color: 'var(--text)' }}>Provider and model</span>
            <span className="min-w-0 truncate" title={`${providerName} · ${modelLabel}`} style={{ color: 'var(--text-2)' }}>{providerName} · {modelLabel}</span>
          </div>

          {effectiveBackend === 'uam' ? (
            <>
              <section
                className="grid gap-1 rounded-lg px-3 py-3"
                style={{
                  border: `1px solid ${active ? 'color-mix(in srgb, var(--accent) 48%, var(--border))' : 'var(--border)'}`,
                  background: active
                    ? 'linear-gradient(135deg, color-mix(in srgb, var(--accent) 18%, var(--surface-up)), color-mix(in srgb, var(--purple) 11%, var(--surface)))'
                    : 'color-mix(in srgb, var(--surface-up) 78%, transparent)',
                }}
              >
                <span className="font-medium" style={{ color: 'var(--text)' }}>{active ? 'AI control approved' : 'Ready for an AI request'}</span>
                <span style={{ color: 'var(--text-3)' }}>
                  {active
                    ? `${targetTitle || 'Approved target'} · ${targetKind === 'screen' ? 'full display' : targetInputMode === 'background' ? 'background window control' : 'foreground window control'}`
                    : 'Ask the AI to use Computer Use. It chooses the target and UAM asks you once to Allow or Deny.'}
                </span>
                {active && (
                  <button
                    type="button"
                    className="uam-choice-button mt-2 inline-flex w-fit items-center gap-1.5 px-2 py-1.5"
                    style={{ color: 'var(--text-2)', border: '1px solid var(--border)' }}
                    disabled={busy}
                    onClick={() => void run(() => onSetActive(false), 'Computer use could not be stopped.')}
                  >
                    <Square size={11} aria-hidden />
                    Stop and revoke access
                  </button>
                )}
              </section>

              <section
                className="grid gap-1 rounded-lg px-3 py-3"
                style={{ border: '1px solid var(--border)', background: 'color-mix(in srgb, var(--surface-up) 78%, transparent)' }}
              >
                <span className="font-medium" style={{ color: 'var(--text)' }}>Privacy and scope</span>
                <span style={{ color: 'var(--text-3)' }}>
                  UAM sends bounded screenshots and accessibility labels from only the approved target to {providerName} ({modelLabel}). Typed content is redacted from UAM action history.
                </span>
              </section>

              <section
                className="grid gap-1 rounded-lg px-3 py-3"
                style={{ border: '1px solid var(--border)', background: 'color-mix(in srgb, var(--surface-up) 78%, transparent)' }}
              >
                <span className="font-medium" style={{ color: 'var(--text)' }}>One target approval</span>
                <span style={{ color: 'var(--text-3)' }}>
                  The AI names the target and UAM asks once before granting it. Allow gives that chat target-scoped control without repeated UAM prompts until you pause, stop, or the target closes.
                </span>
              </section>

              {enabled && (
                <section className="flex items-center gap-2 border-t pt-4" style={{ borderColor: 'var(--border)' }}>
                  <span className="font-medium" style={{ color: 'var(--text)' }}>UAM runtime</span>
                  <span className="capitalize" role="status" style={{ color: state === 'running' ? 'var(--green)' : 'var(--text-3)' }}>{state}</span>
                  <span className="flex-1" />
                  <IconButton
                    size="sm"
                    variant="solid"
                    label={state === 'running' ? 'Pause UAM computer use' : 'Resume UAM computer use'}
                    icon={state === 'running' ? <Pause size={13} aria-hidden /> : <Play size={13} aria-hidden />}
                    disabled={busy}
                    onClick={() => void run(() => onSetControl(state === 'running' ? 'paused' : 'running'), 'The UAM runtime state could not be changed.')}
                  />
                </section>
              )}
            </>
          ) : (
            <section
              className="grid gap-1 rounded-lg px-3 py-3"
              style={{ border: '1px solid var(--border)', background: 'color-mix(in srgb, var(--surface-up) 78%, transparent)' }}
            >
              <span className="font-medium" style={{ color: 'var(--text)' }}>Provider-managed controls</span>
              <span style={{ color: 'var(--text-3)' }}>
                UAM cannot select a target, pause this computer-use session, or show its action history. {providerName} owns app approvals and stop controls. Its policies govern screen-data handling. Turning Active off also stops the structured provider session.
              </span>
            </section>
          )}
        </div>
      </div>
    </div>
  )
}
