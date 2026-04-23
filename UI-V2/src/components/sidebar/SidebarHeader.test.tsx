import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore, type MemoryActivity } from '../../store/useAppStore'
import { SidebarHeader } from './SidebarHeader'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const emptyActivity: MemoryActivity = {
  entryCount: 0,
  lastCreatedAt: '',
  lastCreatedCount: 0,
  runningCount: 0,
  lastStatus: '',
}

function renderSidebarHeader(activity: MemoryActivity = emptyActivity) {
  const openAllMemoryLibrary = vi.fn().mockResolvedValue(true)
  useAppStore.setState({
    memoryActivity: activity,
    openAllMemoryLibrary,
    setNewChatModalOpen: vi.fn(),
    setSettingsOpen: vi.fn(),
  })

  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)

  act(() => {
    root.render(<SidebarHeader />)
  })

  return { host, root, openAllMemoryLibrary }
}

describe('SidebarHeader memory indicator', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    useAppStore.setState({
      memoryActivity: emptyActivity,
      isNewChatModalOpen: false,
      isSettingsOpen: false,
      memoryLibraryScope: null,
      memoryLibraryEntries: [],
      memoryLibraryLoading: false,
      memoryLibraryError: '',
    })
  })

  it('renders muted when no memories exist', () => {
    const { host, root } = renderSidebarHeader()
    const button = host.querySelector('button[title="No memories yet"]') as HTMLButtonElement | null

    expect(button).toBeTruthy()
    expect(button?.style.color).toBe('var(--text-3)')
    expect(host.querySelector('[data-testid="memory-activity-dot"]')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders accent state with a count and timestamp', () => {
    const { host, root } = renderSidebarHeader({
      entryCount: 2,
      lastCreatedAt: '2026-01-01T12:30:00.000Z',
      lastCreatedCount: 0,
      runningCount: 0,
      lastStatus: 'Memory updated.',
    })
    const button = host.querySelector('button[title*="2 memories saved"]') as HTMLButtonElement | null

    expect(button).toBeTruthy()
    expect(button?.title).toContain('last updated')
    expect(button?.style.color).toBe('var(--accent)')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('shows activity when a worker is running or memories were just created', () => {
    const running = renderSidebarHeader({
      entryCount: 1,
      lastCreatedAt: '',
      lastCreatedCount: 0,
      runningCount: 1,
      lastStatus: 'Memory worker started.',
    })

    expect(running.host.querySelector('[data-testid="memory-activity-dot"]')).toBeTruthy()

    act(() => {
      running.root.unmount()
    })
    running.host.remove()

    const justCreated = renderSidebarHeader({
      entryCount: 2,
      lastCreatedAt: '',
      lastCreatedCount: 1,
      runningCount: 0,
      lastStatus: 'Memory updated.',
    })

    expect(justCreated.host.querySelector('[data-testid="memory-activity-dot"]')).toBeTruthy()

    act(() => {
      justCreated.root.unmount()
    })
    justCreated.host.remove()
  })

  it('opens the all memory library on click', () => {
    const { host, root, openAllMemoryLibrary } = renderSidebarHeader({
      entryCount: 1,
      lastCreatedAt: '',
      lastCreatedCount: 0,
      runningCount: 0,
      lastStatus: '',
    })
    const button = host.querySelector('button[title="1 memory saved"]') as HTMLButtonElement

    act(() => {
      button.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(openAllMemoryLibrary).toHaveBeenCalledTimes(1)

    act(() => {
      root.unmount()
    })
    host.remove()
  })
})
