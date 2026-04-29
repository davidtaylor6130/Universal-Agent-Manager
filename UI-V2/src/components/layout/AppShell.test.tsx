import { act } from 'react'
import type { CSSProperties, PropsWithChildren } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'

vi.mock('react-resizable-panels', () => ({
  PanelGroup: ({ children, className }: PropsWithChildren<{ className?: string }>) => (
    <div className={className} data-testid="panel-group">{children}</div>
  ),
  Panel: ({ children, className, style }: PropsWithChildren<{ className?: string; style?: CSSProperties }>) => (
    <section className={className} style={style}>{children}</section>
  ),
  PanelResizeHandle: ({ className, style }: { className?: string; style?: CSSProperties }) => (
    <div className={className} style={style} data-testid="resize-handle" />
  ),
}))

vi.mock('./Sidebar', () => ({
  Sidebar: () => <div data-testid="sidebar">Sidebar</div>,
}))

vi.mock('./MainPanel', () => ({
  MainPanel: () => <div data-testid="main-panel">Main panel</div>,
}))

import { AppShell } from './AppShell'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('AppShell', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    useAppStore.setState({
      isNewChatModalOpen: false,
      isSettingsOpen: false,
      memoryLibraryScope: null,
      isMemoryScanModalOpen: false,
      isMarkdownStoreOpen: false,
    })
  })

  it('renders edge-to-edge without a faux nested window', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<AppShell />)
    })

    expect(host.querySelector('.uam-app')).toBeTruthy()
    expect(host.querySelector('.uam-titlebar')).toBeTruthy()
    expect(host.querySelector('.uam-window')).toBeNull()
    expect(host.querySelector('.uam-window-controls')).toBeNull()
    expect(host.querySelector('.uam-window-dot')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps the top toolbar actions wired', () => {
    const openAllMemoryLibrary = vi.fn().mockResolvedValue(true)
    const setSettingsOpen = vi.fn()
    useAppStore.setState({
      openAllMemoryLibrary,
      setSettingsOpen,
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<AppShell />)
    })

    const memoryButton = host.querySelector('button[aria-label="Memory library"]') as HTMLButtonElement
    const settingsButton = host.querySelector('button[aria-label="Settings"]') as HTMLButtonElement

    act(() => {
      memoryButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      settingsButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(openAllMemoryLibrary).toHaveBeenCalledTimes(1)
    expect(setSettingsOpen).toHaveBeenCalledWith(true)
    expect(Array.from(host.querySelectorAll('button')).some((button) => button.textContent === '+New Chat')).toBe(false)

    act(() => {
      root.unmount()
    })
    host.remove()
  })
})
