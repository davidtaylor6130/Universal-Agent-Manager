import { useEffect, useMemo, useState, type RefObject } from 'react'
import type { Message } from '../../types/message'

/** Message positions stay evenly spaced regardless of message length. */
export function messageIndexAtPointer(count: number, top: number, height: number, pointer: number): number {
  if (count <= 1 || height <= 0) return 0
  return Math.max(0, Math.min(count - 1, Math.round((pointer - top) / height * (count - 1))))
}

export function MessageNavigationRail({ userMessageIndices, startIndex, messages, currentIndex, viewportRef, busy, onNavigate }: {
  userMessageIndices?: number[]
  startIndex: number
  messages: Message[]
  currentIndex: number
  viewportRef: RefObject<HTMLDivElement>
  busy: boolean
  onNavigate: (index: number) => void
}) {
  const indices = useMemo(() => userMessageIndices ?? messages.flatMap((message, index) => message.role === 'user' ? [startIndex + index] : []), [userMessageIndices, messages, startIndex])
  const count = indices.length
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
  if (count === 0) return null
  let currentUserIndex = 0
  let lastUserIndex = count - 1
  while (currentUserIndex < lastUserIndex) {
    const middle = Math.ceil((currentUserIndex + lastUserIndex) / 2)
    if (indices[middle] <= visibleIndex) currentUserIndex = middle
    else lastUserIndex = middle - 1
  }
  const index = Math.max(0, Math.min(count - 1, previewIndex ?? currentUserIndex))
  const message = messages[indices[index] - startIndex]
  const label = `${!userMessageIndices && startIndex > 0 ? 'Loaded user' : 'User'} message ${index + 1} of ${count}`
  const preview = message ? message.content.slice(0, 160) : null
  return <div className="absolute left-0 top-4 z-10 flex w-5 items-center" style={{ bottom: 'calc(var(--composer-dock-h, 0px) + 16px)' }}>
    <div role="slider" tabIndex={busy ? -1 : 0} aria-label="Navigate user messages" aria-orientation="vertical"
      aria-valuemin={1} aria-valuemax={count} aria-valuenow={index + 1} aria-valuetext={label} aria-disabled={busy}
      className="relative w-full cursor-pointer outline-none focus-visible:ring-1 focus-visible:ring-[var(--accent)]"
      style={{ height: Math.max(1, count - 1) * 8, maxHeight: '100%' }}
      onMouseMove={(event) => {
        const rect = event.currentTarget.getBoundingClientRect()
        setPreviewIndex(messageIndexAtPointer(count, rect.top, rect.height, event.clientY))
      }}
      onMouseLeave={() => setPreviewIndex(null)}
      onClick={(event) => {
        if (busy) return
        const rect = event.currentTarget.getBoundingClientRect()
        onNavigate(indices[messageIndexAtPointer(count, rect.top, rect.height, event.clientY)])
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
        onNavigate(indices[target])
      }}>
      <span aria-hidden style={{ position: 'absolute', top: count === 1 ? '50%' : 0, bottom: count === 1 ? undefined : -1, left: 6, width: 14, height: count === 1 ? 1.5 : undefined,
        background: count === 1 ? 'var(--text-3)' : undefined,
        backgroundImage: count > 1 ? 'linear-gradient(to bottom, var(--text-3) 1.5px, transparent 1.5px)' : undefined,
        backgroundSize: count > 1 ? `100% calc((100% - 1px) / ${count - 1})` : undefined }} />
      {previewIndex !== null && [-2, -1, 1, 2].map(distance => {
        const neighbor = index + distance
        if (neighbor < 0 || neighbor >= count) return null
        return <span key={neighbor} aria-hidden data-rail-neighbor={neighbor} className="pointer-events-none motion-reduce:transition-none" style={{ position: 'absolute', top: `${neighbor / (count - 1) * 100}%`, left: 6, width: 48, height: 1.5, background: 'var(--text-3)', transformOrigin: 'left', transition: 'transform var(--dur-fast) var(--ease)', transform: `scaleX(${Math.abs(distance) === 1 ? 2 / 3 : 5 / 12})` }} />
      })}
      <span aria-hidden data-rail-target={index} className="pointer-events-none motion-reduce:transition-none" style={{ position: 'absolute', top: `${count === 1 ? 50 : index / (count - 1) * 100}%`, left: 6, width: 48, height: 4.5, background: 'var(--accent)', transformOrigin: 'left', transition: 'transform var(--dur-fast) var(--ease)', transform: `scaleX(${previewIndex === null ? 22 / 48 : 1})` }} />
      {previewIndex !== null && <div className="conversation-rail-preview" aria-hidden style={{ top: `${count === 1 ? 50 : index / (count - 1) * 100}%` }}>
        <div className="conversation-rail-preview__label">{label}</div>
        {preview !== null
          ? <div className="conversation-rail-preview__text">{preview}</div>
          : <>
            <div className="conversation-rail-preview__title">Not loaded yet</div>
            <div className="conversation-rail-preview__hint">Scroll up or click to load this part of the chat.</div>
          </>}
      </div>}
    </div>
  </div>
}
