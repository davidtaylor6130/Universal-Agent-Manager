import { useEffect, useState, type RefObject } from 'react'
import type { Message } from '../../types/message'

/** Message positions stay evenly spaced regardless of message length. */
export function messageIndexAtPointer(count: number, top: number, height: number, pointer: number): number {
  if (count <= 1 || height <= 0) return 0
  return Math.max(0, Math.min(count - 1, Math.round((pointer - top) / height * (count - 1))))
}

export function MessageNavigationRail({ count, startIndex, messages, currentIndex, viewportRef, busy, onNavigate }: {
  count: number
  startIndex: number
  messages: Message[]
  currentIndex: number
  viewportRef: RefObject<HTMLDivElement>
  busy: boolean
  onNavigate: (index: number) => void
}) {
  const [previewIndex, setPreviewIndex] = useState<number | null>(null)
  const [visibleIndex, setVisibleIndex] = useState(currentIndex)
  useEffect(() => setVisibleIndex(currentIndex), [currentIndex])
  useEffect(() => {
    const viewport = viewportRef.current
    if (!viewport) return
    let frame: number | null = null
    const onScroll = () => {
      if (frame !== null) return
      frame = requestAnimationFrame(() => {
        frame = null
        const top = viewport.getBoundingClientRect().top
        let nearest = startIndex
        for (const element of viewport.querySelectorAll<HTMLElement>('[data-message-index]')) {
          if (element.getBoundingClientRect().top > top + 32) break
          nearest = Number(element.dataset.messageIndex)
        }
        if (Number.isSafeInteger(nearest)) setVisibleIndex(nearest)
      })
    }
    viewport.addEventListener('scroll', onScroll, { passive: true })
    return () => {
      viewport.removeEventListener('scroll', onScroll)
      if (frame !== null) cancelAnimationFrame(frame)
    }
  }, [messages, startIndex, viewportRef])
  if (count < 2) return null
  const index = Math.max(0, Math.min(count - 1, previewIndex ?? visibleIndex))
  const message = messages[index - startIndex]
  const label = `Message ${index + 1} of ${count}`
  const preview = message ? `${message.role}: ${message.content.slice(0, 160)}` : 'Open this message'
  return <div className="absolute left-0 top-4 z-10 w-5" style={{ bottom: 'calc(var(--composer-dock-h, 0px) + 16px)' }}>
    <div role="slider" tabIndex={busy ? -1 : 0} aria-label="Navigate messages" aria-orientation="vertical"
      aria-valuemin={1} aria-valuemax={count} aria-valuenow={index + 1} aria-valuetext={label} aria-disabled={busy}
      className="relative h-full cursor-pointer outline-none focus-visible:ring-1 focus-visible:ring-[var(--accent)]"
      title={`${label}\n${preview}`}
      onMouseMove={(event) => {
        const rect = event.currentTarget.getBoundingClientRect()
        setPreviewIndex(messageIndexAtPointer(count, rect.top, rect.height, event.clientY))
      }}
      onMouseLeave={() => setPreviewIndex(null)}
      onClick={(event) => {
        if (busy) return
        const rect = event.currentTarget.getBoundingClientRect()
        onNavigate(messageIndexAtPointer(count, rect.top, rect.height, event.clientY))
      }}
      onKeyDown={(event) => {
        if (busy) return
        const deltas: Record<string, number> = { ArrowUp: -1, ArrowDown: 1, PageUp: -10, PageDown: 10 }
        const next = event.key === 'Home' ? 0 : event.key === 'End' ? count - 1
          : event.key in deltas ? Math.max(0, Math.min(count - 1, index + deltas[event.key])) : null
        if (next === null && event.key !== 'Enter' && event.key !== ' ') return
        event.preventDefault()
        const target = next ?? index
        setPreviewIndex(target)
        onNavigate(target)
      }}>
      <span aria-hidden style={{ position: 'absolute', top: 0, bottom: 0, left: 9, width: 1, background: 'var(--border)' }} />
      {messages.filter((message) => message.role === 'user').slice(0, 64).map((message) => {
        const absoluteIndex = startIndex + messages.indexOf(message)
        return <span key={message.id} aria-hidden style={{ position: 'absolute', top: `${absoluteIndex / (count - 1) * 100}%`, left: 6, width: 7, height: 2, background: 'var(--text-3)' }} />
      })}
      <span aria-hidden style={{ position: 'absolute', top: `${index / (count - 1) * 100}%`, left: 4, width: 11, height: 3, background: 'var(--accent)' }} />
    </div>
  </div>
}
