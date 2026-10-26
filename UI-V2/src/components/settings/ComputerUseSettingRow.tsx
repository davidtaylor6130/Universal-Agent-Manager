import { useEffect, useId, useLayoutEffect, useRef } from 'react'
import { COMPUTER_USE_ENABLED } from '../../config/buildFeatures'
import { sendToCEF } from '../../ipc/cefBridge'
import { useAppStore } from '../../store/useAppStore'
import { Button, Switch } from '../ui'
import { SettingRow } from './SettingsPrimitives'

type ComputerUseSettings = { computerUseGloballyEnabled?: boolean; computerUseRevocationPending?: boolean }
type ComputerUseObservation = ComputerUseSettings & { revision?: number }
let operationInFlight = false

function currentObservation(): ComputerUseObservation {
  const state = useAppStore.getState()
  return { computerUseGloballyEnabled: state.computerUseEnabled, computerUseRevocationPending: state.computerUseRevocationPending, revision: state.computerUseSettingsStateRevision }
}
function sameObservation(left: ComputerUseObservation, right: ComputerUseObservation): boolean {
  return left.revision === right.revision && left.computerUseGloballyEnabled === right.computerUseGloballyEnabled && left.computerUseRevocationPending === right.computerUseRevocationPending
}

/** Global consent is separate from OS permission and each chat's exact target grant. */
export function ComputerUseSettingRow({ disabled = false }: { disabled?: boolean }) {
  const enabled = useAppStore(state => state.computerUseEnabled)
  const pending = useAppStore(state => state.computerUseRevocationPending)
  const busy = useAppStore(state => state.computerUseSettingsOperation)
  const retainedError = useAppStore(state => state.computerUseSettingsError ?? '')
  const errorRevision = useAppStore(state => state.computerUseSettingsErrorStateRevision)
  const revision = useAppStore(state => state.computerUseSettingsStateRevision)
  const error = busy || revision === errorRevision ? retainedError : ''
  const setError = (message: string) => useAppStore.setState({ computerUseSettingsError: message, computerUseSettingsErrorStateRevision: useAppStore.getState().computerUseSettingsStateRevision })
  const controls = useRef<HTMLDivElement>(null)
  const focusAfterRecovery = useRef<HTMLInputElement | HTMLButtonElement | null>(null)
  const statusId = useId()
  const errorId = useId()
  const known = typeof enabled === 'boolean' && typeof pending === 'boolean'
  const describedBy = `${statusId}${error ? ` ${errorId}` : ''}`

  useEffect(() => {
    if (!busy && retainedError && revision !== errorRevision) {
      useAppStore.setState({ computerUseSettingsError: '', computerUseSettingsErrorStateRevision: undefined })
    }
  }, [busy, retainedError, revision, errorRevision])
  useLayoutEffect(() => {
    if (busy || !focusAfterRecovery.current) return
    const trigger = focusAfterRecovery.current
    focusAfterRecovery.current = null
    // Do not steal focus from navigation or another control during an async request.
    if (document.activeElement !== document.body && document.activeElement !== trigger) return
    const destination = trigger.isConnected && !trigger.disabled ? trigger
      : controls.current?.querySelector<HTMLElement>('input:not(:disabled), button:not(:disabled)')
    destination?.focus()
  }, [busy, known, pending])

  const observe = (data: ComputerUseSettings | undefined, captured: ComputerUseObservation): 'applied' | 'superseded' | 'invalid' => {
    if (typeof data?.computerUseGloballyEnabled !== 'boolean' || typeof data.computerUseRevocationPending !== 'boolean') return 'invalid'
    // A native push observed since this request takes precedence over its older response.
    if (!sameObservation(currentObservation(), captured)) return 'superseded'
    useAppStore.setState({ computerUseEnabled: data.computerUseGloballyEnabled, computerUseRevocationPending: data.computerUseRevocationPending })
    return 'applied'
  }
  const check = async () => {
    const captured = currentObservation()
    const response = await sendToCEF<ComputerUseSettings>({ action: 'getComputerUseSettings' })
    if (!response.ok || observe(response.data, captured) === 'invalid') throw Error(response.error || 'Could not confirm Computer Use status.')
  }
  const change = async (next?: boolean, trigger?: HTMLInputElement | HTMLButtonElement) => {
    if (operationInFlight || disabled || !COMPUTER_USE_ENABLED) return
    const captured = currentObservation()
    if (next === true && (typeof captured.computerUseGloballyEnabled !== 'boolean' || captured.computerUseRevocationPending !== false)) return
    if (trigger && document.activeElement === trigger) focusAfterRecovery.current = trigger
    operationInFlight = true
    useAppStore.setState({ computerUseSettingsOperation: next === undefined ? 'check' : next ? 'enable' : 'disable' })
    setError('')
    try {
      if (next === undefined) await check()
      else {
        const response = await sendToCEF<ComputerUseSettings>({ action: 'setComputerUseSettings', payload: { computerUseGloballyEnabled: next } })
        if (!response.ok) throw Error(response.error || 'Could not change Computer Use.')
        if (observe(response.data, captured) !== 'applied') await check()
      }
    } catch (failure) {
      const message = failure instanceof Error ? failure.message : 'Could not change Computer Use.'
      // Off can persist even when native shutdown fails, so re-read instead of rolling back.
      let confirmed = false
      if (next !== undefined) {
        try { await check(); confirmed = true } catch { /* Keep the outcome unknown until a successful read. */ }
      }
      if (!confirmed && !sameObservation(currentObservation(), captured)) confirmed = true
      if (!confirmed) {
        useAppStore.setState({ computerUseEnabled: undefined, computerUseRevocationPending: undefined })
      }
      setError(confirmed ? message : `${message} Status is not confirmed; check before trying again.`)
    } finally {
      operationInFlight = false
      useAppStore.setState({ computerUseSettingsOperation: undefined })
    }
  }

  const status = !COMPUTER_USE_ENABLED ? 'Unavailable in this build'
    : busy === 'disable' ? 'Turning off and clearing UAM grants…'
    : busy === 'enable' ? 'Turning on…'
    : busy === 'check' ? 'Checking status…'
    : pending && enabled === true ? "On. A chat's controls are still stopping. Turn off for all chats to retry cleanup."
    : pending ? known ? 'Off. Cleanup pending. Retry disabling before enabling.' : 'Cleanup pending. Global status not confirmed.'
    : !known ? 'Status not confirmed.'
    : enabled ? 'On. Each chat still needs an approved target.' : 'Off. UAM grants cleared.'

  return <SettingRow title="UAM Computer Use" description={COMPUTER_USE_ENABLED ? 'Allow target requests on this computer. Saves immediately.' : 'Computer Use is unavailable in this build.'}
    status={<span id={statusId} role="status" aria-live="polite">{status}</span>}
    control={COMPUTER_USE_ENABLED && <div ref={controls} className="flex flex-wrap items-center justify-end gap-2">
      {known && <Switch className="uam-switch--accent" hideLabel label="UAM Computer Use" checked={enabled}
        aria-describedby={describedBy} disabled={disabled || Boolean(busy) || (pending && !enabled)}
        onChange={event => void change(event.currentTarget.checked, event.currentTarget)} />}
      {(pending || !known) && <Button size="sm" aria-describedby={describedBy} disabled={disabled || Boolean(busy)} onClick={event => void change(false, event.currentTarget)}>{pending && enabled === false ? 'Retry disabling' : 'Turn off'}</Button>}
      {!known && <Button size="sm" aria-describedby={describedBy} disabled={disabled || Boolean(busy)} onClick={event => void change(undefined, event.currentTarget)}>Check status</Button>}
    </div>}>
    {error && <p id={errorId} role="alert" className="text-xs" style={{ color: 'var(--error)' }}>{error}</p>}
  </SettingRow>
}
