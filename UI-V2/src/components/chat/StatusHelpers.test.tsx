import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, expect, it, vi } from 'vitest'
import { CHAT_ANNOUNCEMENT_CLEAR_MS, ChatAnnouncer, ChatStatusStrip, CopyTextButton, ReplyAnnouncement, replyOutcomeAnnouncement } from './StatusHelpers'
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

const announcement = (host: HTMLElement) => host.querySelector('[data-testid="chat-announcement"]')!

it('announces a status divider once when it opens and never its ticking timer', () => {
  vi.useFakeTimers()
  vi.stubGlobal('matchMedia', () => ({ matches: true }))
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  const render = (open: boolean) => act(() => root.render(
    <ChatAnnouncer><ChatStatusStrip open={open} tone="info" scene={null} title="Compacting context" /></ChatAnnouncer>,
  ))
  render(false)
  const region = announcement(host)
  expect(region.getAttribute('aria-live')).toBe('polite')
  expect(region.textContent).toBe('')
  render(true)
  const strip = host.querySelector('.chat-status-strip')!
  expect(strip.getAttribute('role')).toBeNull()
  expect(strip.closest('[aria-live]')).toBeNull()
  expect(region.contains(strip)).toBe(false)
  expect(region.textContent).toBe('Compacting context')
  const announced = region.firstChild
  const mutations: MutationRecord[] = []
  const observer = new MutationObserver((records) => mutations.push(...records))
  observer.observe(region, { childList: true, subtree: true, characterData: true })
  act(() => vi.advanceTimersByTime(3000))
  expect(strip.textContent).toContain('3s')
  observer.disconnect()
  expect(mutations).toEqual([])
  expect(region.firstChild).toBe(announced)
  act(() => root.unmount())
  host.remove()
})

it('does not announce a divider that was already open when the chat opened', () => {
  vi.stubGlobal('matchMedia', () => ({ matches: true }))
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  act(() => root.render(<ChatAnnouncer><ChatStatusStrip open tone="agents" scene={null} title="Waiting on subagents" /></ChatAnnouncer>))
  expect(announcement(host).textContent).toBe('')
  act(() => root.unmount())
  host.remove()
})

it.each([
  { failed: false, stopReason: undefined, interrupted: false, line: 'Reply finished.' },
  { failed: true, stopReason: undefined, interrupted: false, line: 'Reply failed.' },
  { failed: false, stopReason: 'failed', interrupted: false, line: 'Reply failed.' },
  { failed: true, stopReason: 'timeout', interrupted: false, line: 'Reply stopped.' },
  { failed: false, stopReason: undefined, interrupted: true, line: 'Reply stopped.' },
])('reads one outcome line: $line', ({ line, ...outcome }) => {
  expect(replyOutcomeAnnouncement(outcome)).toBe(line)
})

it('announces a reply starting, waiting, finishing and failing once each, but not the state it opened in', () => {
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  const region = () => announcement(host)
  const lines: string[] = []
  let last: ChildNode | null = null
  // Each announcement is a fresh node; record only renders that produced one.
  const render = (props: Partial<Parameters<typeof ReplyAnnouncement>[0]>) => {
    act(() => root.render(<ChatAnnouncer><ReplyAnnouncement working={false} failed={false} interrupted={false} {...props} /></ChatAnnouncer>))
    if (region().firstChild !== last) { last = region().firstChild; if (last) lines.push(last.textContent ?? '') }
  }
  render({ working: true })
  expect(lines).toEqual([])
  render({ working: false })
  render({ working: true })
  render({ working: true, waitingFor: 'permission' })
  render({ working: true })
  render({ working: true, waitingFor: 'answer' })
  render({ working: false, failed: true })
  render({ working: false, failed: true })
  render({ working: true })
  render({ working: false })
  expect(lines).toEqual(['Reply finished.', 'Reply started.', 'Waiting for your permission.', 'Waiting for your answer.', 'Reply failed.', 'Reply started.', 'Reply finished.'])
  act(() => root.unmount())
  host.remove()
})

it('reads the same line twice as two announcements, skips an outcome a notice already carries and clears after a while', () => {
  vi.useFakeTimers()
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  const render = (props: Partial<Parameters<typeof ReplyAnnouncement>[0]>) => act(() => root.render(
    <ChatAnnouncer><ReplyAnnouncement working={false} failed={false} interrupted={false} {...props} /></ChatAnnouncer>,
  ))
  render({ working: true })
  render({ working: false })
  const first = announcement(host).firstChild
  expect(first?.textContent).toBe('Reply finished.')
  render({ working: true })
  render({ working: false })
  expect(announcement(host).firstChild?.textContent).toBe('Reply finished.')
  expect(announcement(host).firstChild).not.toBe(first)
  render({ working: true })
  render({ working: false, failed: true, noticed: true })
  expect(announcement(host).textContent).toBe('Reply started.')
  act(() => vi.advanceTimersByTime(CHAT_ANNOUNCEMENT_CLEAR_MS))
  expect(announcement(host).textContent).toBe('')
  act(() => root.unmount())
  host.remove()
})
