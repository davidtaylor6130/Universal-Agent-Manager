import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, expect, it, vi } from 'vitest'
import { ComputerUseModal } from './ComputerUseModal'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

afterEach(() => {
  document.body.innerHTML = ''
})

it('leaves UAM target choice to the AI and exposes only revocation after approval', async () => {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  const onSetActive = vi.fn(async () => ({ ok: true as const }))
  const props = {
    active: false,
    enabled: false,
    disabled: false,
    backend: 'uam' as const,
    effectiveBackend: 'uam' as const,
    providerAvailable: false,
    providerName: 'OpenCode',
    modelLabel: 'Qwen',
    targetKind: 'window' as const,
    targetTitle: '',
    targetInputMode: 'foreground' as const,
    state: 'stopped' as const,
    onClose: vi.fn(),
    onSetActive,
    onSetBackend: vi.fn(async () => ({ ok: true as const })),
    onSetControl: vi.fn(async () => ({ ok: true as const })),
  }

  await act(async () => root.render(<ComputerUseModal {...props} />))
  expect(host.textContent).toContain('Ready for an AI request')
  expect(host.textContent).toContain('It chooses the target')
  expect(host.querySelector('[aria-label="Computer-use target"]')).toBeNull()
  expect(host.querySelector('[aria-label="Computer use active"]')).toBeNull()

  await act(async () => root.render(<ComputerUseModal {...props} active enabled targetTitle="TextEdit" state="running" />))
  const stop = Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Stop and revoke access'))
  expect(stop).toBeTruthy()
  await act(async () => stop?.click())
  expect(onSetActive).toHaveBeenCalledWith(false)

  await act(async () => root.render(<ComputerUseModal {...props} disabled remoteDisabled />))
  expect(host.textContent).toContain('Remote Computer Use is disabled')
  expect(host.textContent).toContain('UAM will reject Computer Use requests for this remote chat')
  expect(host.textContent).not.toContain('Ready for an AI request')
  expect(host.textContent).not.toContain('One target approval')
  expect(host.querySelector<HTMLSelectElement>('#computer-use-backend')?.disabled).toBe(true)

  await act(async () => root.unmount())
})
