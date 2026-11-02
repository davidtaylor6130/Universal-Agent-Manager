import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { expect, it, vi } from 'vitest'
import { formatTimeoutDuration, TimeoutStopNotice, type TimeoutRestartOutcome } from './TimeoutStopNotice'
import { sanitizeCppAcpSession } from '../../store/cpp/sanitizers'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

it('explains a timeout stop with its name and value and restarts with one action', async () => {
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  let finish: (outcome?: TimeoutRestartOutcome) => void = () => {}
  const onRestart = vi.fn(() => new Promise<TimeoutRestartOutcome | undefined>((resolve) => { finish = resolve }))
  try {
    act(() => root.render(<TimeoutStopNotice timeoutStop={{ label: 'OpenCode request timeout', seconds: 1800 }} onRestart={onRestart} onDismiss={() => {}} />))
    expect(host.textContent).toContain('Turn timed out')
    expect(host.textContent).toContain('OpenCode request timeout (30 min) expired')
    expect(host.textContent).toContain('Nothing was lost')
    const restart = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Restart with +15 min')!
    act(() => restart.click())
    expect(onRestart).toHaveBeenCalledTimes(1)
    expect(restart.disabled).toBe(true)
    await act(async () => { finish({ tone: 'info', message: 'Timeout raised to 45 min. This turn already produced output, so send a follow-up to continue it.' }) })
    expect(restart.disabled).toBe(false)
    const notice = host.querySelector('[role="status"].uam-notice')!
    expect(notice.textContent).toContain('Timeout raised to 45 min')
    expect(notice.querySelectorAll('[role="status"], [role="alert"]')).toHaveLength(0)
    act(() => restart.click())
    await act(async () => { finish({ tone: 'error', message: 'The turn did not restart: refused.' }) })
    const alert = host.querySelector('[role="alert"]')!
    expect(alert.textContent).toContain('did not restart')
    expect(notice.contains(alert)).toBe(false)
  } finally {
    act(() => root.unmount())
    host.remove()
  }
})

it('explains a provider limit UAM does not own without a value or Restart', () => {
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  try {
    act(() => root.render(<TimeoutStopNotice timeoutStop={{ label: 'An OpenCode provider timeout', seconds: 0 }} onRestart={vi.fn()} onDismiss={() => {}} />))
    expect(host.textContent).toContain('An OpenCode provider timeout expired')
    expect(host.textContent).toContain('provider’s own configuration')
    expect(Array.from(host.querySelectorAll('button')).some((button) => button.textContent === 'Restart with +15 min')).toBe(false)
  } finally {
    act(() => root.unmount())
    host.remove()
  }
})

it('formats timeout values and reads the native timeout stop', () => {
  expect(formatTimeoutDuration(900)).toBe('15 min')
  expect(formatTimeoutDuration(3600)).toBe('1 h')
  expect(formatTimeoutDuration(4500)).toBe('1 h 15 min')
  expect(sanitizeCppAcpSession({ timeoutStop: { label: 'Provider turn timeout', seconds: 2700 } })?.timeoutStop).toEqual({ label: 'Provider turn timeout', seconds: 2700 })
  expect(sanitizeCppAcpSession({ timeoutStop: null })?.timeoutStop).toBeUndefined()
})
