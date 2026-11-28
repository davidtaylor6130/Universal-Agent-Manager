import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, describe, expect, it } from 'vitest'
import { PermissionInlineCard } from './ToolCallViews'
import type { AcpPendingPermission } from '../../store/cpp/types'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const permission: AcpPendingPermission = {
  requestId: '7',
  toolCallId: 'tool-7',
  title: 'git push origin main',
  kind: 'execute',
  status: 'pending',
  content: 'git push origin main',
  options: [{ id: 'allow-once', name: 'Allow once', kind: 'allow_once' }, { id: 'reject-once', name: 'Reject', kind: 'reject_once' }],
}

describe('Permission rule notice', () => {
  let host: HTMLDivElement
  let root: Root
  afterEach(() => { act(() => root.unmount()); host.remove() })
  const show = (value: AcpPendingPermission) => {
    host = document.createElement('div')
    document.body.appendChild(host)
    root = createRoot(host)
    act(() => root.render(<PermissionInlineCard permission={value} onResolve={async () => true} />))
  }

  it('shows one line when a rule holds a remote commit for approval', () => {
    show({ ...permission, ruleNotice: 'Pushes, merges and other remote commits always wait for your approval.' })
    const notice = document.querySelector('[data-testid="permission-rule-notice"]')
    expect(notice?.textContent).toBe('Pushes, merges and other remote commits always wait for your approval.')
  })

  it('shows no notice for an ordinary request', () => {
    show(permission)
    expect(document.querySelector('[data-testid="permission-rule-notice"]')).toBeNull()
  })
})
