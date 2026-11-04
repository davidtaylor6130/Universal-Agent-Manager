import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { DeleteConfirmDialog } from './DeleteConfirmDialog'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

let cleanup = () => {}
afterEach(() => cleanup())

function render(props: Omit<Parameters<typeof DeleteConfirmDialog>[0], 'open' | 'onConfirm' | 'onCancel'>) {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  const onCancel = vi.fn()
  act(() => root.render(<DeleteConfirmDialog open onConfirm={vi.fn()} onCancel={onCancel} {...props} />))
  cleanup = () => { act(() => root.unmount()); host.remove() }
  const dialog = document.body.querySelector<HTMLElement>('[role="alertdialog"]')!
  return { dialog, onCancel, buttons: Array.from(dialog.querySelectorAll('button')).map((button) => button.textContent) }
}

describe('DeleteConfirmDialog', () => {
  it.each([
    [{ kind: 'chat' as const, count: 1, name: 'Plan', contents: 'its 2 related branches' }, 'Delete chat Plan and its 2 related branches', 'Delete chat?', 'The chat “Plan” and its 2 related branches will be permanently deleted.', 'Delete chat'],
    [{ kind: 'chat' as const, count: 3 }, 'Delete 3 selected chats', 'Delete 3 selected chats?', '3 chats will be permanently deleted.', 'Delete chats'],
    [{ kind: 'workspace' as const, count: 1, name: 'Repo', contents: '1 chat', title: 'Delete workspace and chats?' }, 'Delete workspace Repo and 1 chat', 'Delete workspace and chats?', 'The workspace “Repo” and 1 chat will be permanently deleted.', 'Delete workspace'],
    [{ kind: 'workspace' as const, count: 2, contents: '5 chats' }, 'Delete 2 selected workspaces', 'Delete 2 selected workspaces?', '2 workspaces and 5 chats will be permanently deleted.', 'Delete workspaces'],
    [{ kind: 'collection' as const, count: 2 }, 'Delete 2 selected collections', 'Delete 2 selected collections?', '2 collections will be permanently deleted.', 'Delete collections'],
  ])('names type, count and contents with one layout (%o)', (props, label, title, message, confirm) => {
    const { dialog, buttons, onCancel } = render(props)
    expect(dialog.getAttribute('aria-label')).toBe(label)
    expect(dialog.textContent).toContain(title)
    expect(dialog.textContent).toContain(`${message} This cannot be undone.`)
    expect(document.getElementById(dialog.getAttribute('aria-describedby')!)?.textContent).toContain(message)
    const status = dialog.querySelector('[role="status"][aria-live="polite"]')
    expect(status).not.toBeNull()
    expect(status!.textContent).toBe('')
    expect(buttons).toEqual(['Cancel', confirm])
    expect(document.activeElement?.textContent).toBe('Cancel')
    act(() => document.activeElement?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(onCancel).toHaveBeenCalledOnce()
  })

  it('reports progress while busy and keeps Escape from cancelling', () => {
    const { dialog, onCancel } = render({ kind: 'workspace', count: 1, contents: '7 chats', busy: true })
    expect(dialog.querySelector('[role="status"]')?.textContent).toBe('Deleting 1 workspace and 7 chats…')
    act(() => document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(onCancel).not.toHaveBeenCalled()
  })
})
