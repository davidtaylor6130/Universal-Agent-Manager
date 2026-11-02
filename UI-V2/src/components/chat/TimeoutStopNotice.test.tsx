import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { expect, it, vi } from 'vitest'
import { formatTimeoutDuration, TimeoutStopNotice } from './TimeoutStopNotice'
import { sanitizeCppAcpSession } from '../../store/cpp/sanitizers'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

it('explains a timeout stop with its name and value and restarts with one action', async () => {
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  let finish: (message?: string) => void = () => {}
  const onRestart = vi.fn(() => new Promise<string | undefined>((resolve) => { finish = resolve }))
  try {
    act(() => root.render(<TimeoutStopNotice timeoutStop={{ label: 'OpenCode request timeout', seconds: 1800 }} onRestart={onRestart} onDismiss={() => {}} />))
    expect(host.textContent).toContain('Turn timed out')
    expect(host.textContent).toContain('OpenCode request timeout (30 min) expired')
    expect(host.textContent).toContain('Nothing was lost')
    const restart = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Restart with +15 min')!
    act(() => restart.click())
    expect(onRestart).toHaveBeenCalledTimes(1)
    expect(restart.disabled).toBe(true)
    await act(async () => { finish('Timeout raised to 45 min. This turn already produced output, so send a follow-up to continue it.') })
    expect(restart.disabled).toBe(false)
    expect(host.querySelector('p[role="status"]')?.textContent).toContain('45 min')
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
