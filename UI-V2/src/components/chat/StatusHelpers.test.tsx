import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, expect, it, vi } from 'vitest'
import { ChatStatusStrip, CopyTextButton } from './StatusHelpers'
import * as clipboard from '../../utils/copySelection'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

afterEach(() => {
  document.documentElement.removeAttribute('data-motion')
  document.documentElement.style.removeProperty('--motion-scale')
  vi.useRealTimers()
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
})

it.each([true, false])('shows the correct copy feedback when clipboard success is %s', async (success) => {
  vi.spyOn(clipboard, 'copyTextToClipboard').mockResolvedValue(success)
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  act(() => root.render(<CopyTextButton text="example" />))
  expect(host.querySelectorAll('.uam-copy-feedback [data-visible="true"]')).toHaveLength(1)
  await act(async () => host.querySelector('button')!.click())
  expect(host.querySelectorAll('.uam-copy-feedback [data-visible="true"]')).toHaveLength(1)
  const announcement = host.querySelector('[role="status"]')!
  expect(host.querySelector('button')!.contains(announcement)).toBe(false)
  expect(announcement.textContent).toBe(success ? 'Copied' : 'Copy failed')
  act(() => root.unmount())
  host.remove()
})

it.each([
  { mode: 'slow', scale: '1.5', exit: 255 },
  { mode: 'off', scale: '0', exit: 0 },
])('keeps the status exit aligned with $mode motion', ({ mode, scale, exit }) => {
  vi.useFakeTimers()
  vi.stubGlobal('matchMedia', () => ({ matches: false }))
  document.documentElement.setAttribute('data-motion', mode)
  document.documentElement.style.setProperty('--motion-scale', scale)
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  const render = (open: boolean) => act(() => root.render(
    <ChatStatusStrip open={open} tone="info" scene={null} title="Working" />,
  ))
  render(true)
  render(false)
  if (exit) {
    expect(host.querySelector('[data-state="closed"]')).not.toBeNull()
    act(() => vi.advanceTimersByTime(exit - 1))
    expect(host.querySelector('[data-state="closed"]')).not.toBeNull()
    act(() => vi.advanceTimersByTime(1))
  }
  expect(host.querySelector('.chat-status-strip')).toBeNull()
  act(() => root.unmount())
  host.remove()
})

it('keeps the elapsed value during exit and cancels removal on reopen', () => {
  vi.useFakeTimers()
  vi.stubGlobal('matchMedia', () => ({ matches: false }))
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  const render = (open: boolean) => act(() => root.render(
    <ChatStatusStrip open={open} tone="info" scene={null} title="Working" />,
  ))
  render(true)
  act(() => vi.advanceTimersByTime(2000))
  expect(host.textContent).toContain('2s')
  render(false)
  expect(host.textContent).toContain('2s')
  act(() => vi.advanceTimersByTime(100))
  render(true)
  act(() => vi.advanceTimersByTime(100))
  expect(host.querySelector('[data-state="open"]')).not.toBeNull()
  act(() => root.unmount())
  host.remove()
})

it('removes the strip immediately when the OS requests reduced motion', () => {
  vi.stubGlobal('matchMedia', () => ({ matches: true }))
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  const render = (open: boolean) => act(() => root.render(
    <ChatStatusStrip open={open} tone="info" scene={null} title="Working" />,
  ))
  render(true)
  render(false)
  expect(host.querySelector('.chat-status-strip')).toBeNull()
  act(() => root.unmount())
  host.remove()
})
