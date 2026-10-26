import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'

const bridge = vi.hoisted(() => ({ send: vi.fn() }))
vi.mock('../../ipc/cefBridge', async (original) => ({ ...await original<typeof import('../../ipc/cefBridge')>(), isCompanionContext: () => false, sendToCEF: bridge.send }))
import { LegacyChatImportPrompt } from './LegacyChatImportPrompt'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const sources = [{ path: 'C:\\old\\data', chatCount: 3 }, { path: 'C:\\Users\\me\\AppData\\Local\\Universal Agent Manager', chatCount: 1 }]

async function mount(resolve: (request: { action: string; payload?: unknown }) => unknown) {
  bridge.send.mockImplementation(async (request: { action: string; payload?: unknown }) =>
    request.action === 'getLegacyChatImports' ? { ok: true, data: { sources } } : resolve(request))
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  await act(async () => root.render(<LegacyChatImportPrompt />))
  return { dialog: () => document.querySelector('[role="alertdialog"]'), close: () => { act(() => root.unmount()); host.remove() } }
}

function button(label: string) {
  return Array.from(document.querySelectorAll('button')).find((item) => item.textContent === label)!
}

afterEach(() => bridge.send.mockReset())

describe('LegacyChatImportPrompt', () => {
  it('copies each found history in turn and closes after the last answer', async () => {
    const view = await mount(async () => ({ ok: true, data: { status: 'complete', importedCount: 3, failedCount: 0, errors: [] } }))
    expect(view.dialog()?.textContent).toContain('Found 3 chats from an earlier version in C:\\old\\data.')
    await act(async () => button('Copy chats').click())
    expect(bridge.send).toHaveBeenLastCalledWith({ action: 'resolveLegacyChatImport', payload: { path: 'C:\\old\\data', choice: 'copy' } })
    expect(view.dialog()?.textContent).toContain('Found 1 chat from an earlier version')
    await act(async () => button('Brand new').click())
    expect(bridge.send).toHaveBeenLastCalledWith({ action: 'resolveLegacyChatImport', payload: { path: sources[1].path, choice: 'brandNew' } })
    expect(view.dialog()).toBeNull()
    view.close()
  })

  it('keeps the choice open with the error when a copy is incomplete', async () => {
    const view = await mount(async () => ({ ok: true, data: { status: 'degraded', importedCount: 2, failedCount: 1, errors: ['Could not import previous-version chat x'] } }))
    await act(async () => button('Copy chats').click())
    expect(view.dialog()?.querySelector('[role="alert"]')?.textContent).toBe('Could not import previous-version chat x')
    expect(view.dialog()?.textContent).toContain('C:\\old\\data')
    view.close()
  })

  it('ignores Escape and asks native to close UAM on Stop', async () => {
    const view = await mount(async () => ({ ok: true, data: {} }))
    await act(async () => { document.activeElement?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })) })
    expect(view.dialog()).not.toBeNull()
    expect(bridge.send).toHaveBeenCalledTimes(1)
    await act(async () => button('Stop').click())
    expect(bridge.send).toHaveBeenLastCalledWith({ action: 'quitApplication' }, false)
    view.close()
  })
})
