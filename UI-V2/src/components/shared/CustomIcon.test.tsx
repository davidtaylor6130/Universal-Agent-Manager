import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { CustomIcon, useCustomIconPicker } from './CustomIcon'
import { useAppStore } from '../../store/useAppStore'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('Custom icons', () => {
  it('falls from failed workspace PNG to host then default', () => {
    const host = document.createElement('div'); const root = createRoot(host)
    act(() => root.render(<CustomIcon icon={{ type: 'png', value: 'workspace.png', dataUrl: 'data:image/png;base64,AAAA' }} secondary={{ type: 'text', value: 'H' }} fallback={<span>Default</span>} />))
    act(() => host.querySelector('img')!.dispatchEvent(new Event('error')))
    expect(host.textContent).toBe('H')
    act(() => root.render(<CustomIcon icon={{ type: 'png', value: 'workspace.png', dataUrl: 'data:image/png;base64,AAAA' }} fallback={<span>Default</span>} />))
    expect(host.textContent).toBe('Default')
    act(() => root.unmount())
  })

  it('saves the actual target through the shared dialog and preserves failed saves', async () => {
    const previous = useAppStore.getState().setCustomIcon
    const save = vi.fn().mockResolvedValueOnce(false).mockResolvedValueOnce(true)
    useAppStore.setState({ setCustomIcon: save })
    function Harness() { const picker = useCustomIconPicker(); return <><button onClick={() => picker.open('desktop-app', 'ref-1', 'Editor')}>Icon</button>{picker.dialog}</> }
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    act(() => root.render(<Harness />)); act(() => host.querySelector('button')!.click())
    const dialog = document.body.querySelector('[role="dialog"]')!
    await act(async () => Array.from(dialog.querySelectorAll('button')).find(button => button.textContent === 'Reset')!.click())
    expect(dialog.textContent).toContain('Icon could not be saved.')
    expect(save).toHaveBeenCalledWith('desktop-app', 'ref-1', null)
    await act(async () => Array.from(dialog.querySelectorAll('button')).find(button => button.textContent === 'Reset')!.click())
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
    act(() => root.unmount()); host.remove(); useAppStore.setState({ setCustomIcon: previous })
  })
})
