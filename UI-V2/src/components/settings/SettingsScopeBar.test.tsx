import { act, createElement } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { SettingsScopeBar } from './SettingsScopeBar'
import type { ExecutionHost } from '../../types/session'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const hosts = [
  { id: 'local', label: 'This computer', transport: 'local' },
  { id: 'box-1', label: 'Build box', transport: 'ssh', sshAlias: 'build' },
] as ExecutionHost[]

function render(props: { value: string; supported: boolean; selectorEnabled?: boolean; onChange?: (id: string) => void }) {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  act(() => root.render(createElement(SettingsScopeBar, { hosts, onChange: vi.fn(), selectorEnabled: props.supported, ...props })))
  return { host, root, cleanup: () => { act(() => root.unmount()); host.remove() } }
}

describe('SettingsScopeBar', () => {
  it('is disabled and shows All environments until the backend supports per-computer settings', () => {
    const view = render({ value: 'box-1', supported: false })
    try {
      const trigger = view.host.querySelector('button#settings-scope') as HTMLButtonElement
      expect(trigger.disabled).toBe(true)
      expect(trigger.textContent).toContain('All environments')
      expect(view.host.textContent).not.toContain('Changes here apply only to')
    } finally { view.cleanup() }
  })

  it('shows the host hint when a host is in scope and supported', () => {
    const view = render({ value: 'box-1', supported: true })
    try {
      const trigger = view.host.querySelector('button#settings-scope') as HTMLButtonElement
      expect(trigger.disabled).toBe(false)
      expect(trigger.textContent).toContain('Build box')
      expect(view.host.textContent).toContain('Changes here apply only to Build box.')
    } finally { view.cleanup() }
  })

  it('stays enabled on provider pages while saves remain unscoped without the backend flag', () => {
    const view = render({ value: 'box-1', supported: false, selectorEnabled: true })
    try {
      const trigger = view.host.querySelector('button#settings-scope') as HTMLButtonElement
      expect(trigger.disabled).toBe(false)
      expect(view.host.textContent).toContain('Defaults and MCP lists still apply to all environments')
    } finally { view.cleanup() }
  })

  it('shows no hint for All environments', () => {
    const view = render({ value: '*', supported: true })
    try {
      expect(view.host.textContent).not.toContain('Changes here apply only to')
    } finally { view.cleanup() }
  })
})
