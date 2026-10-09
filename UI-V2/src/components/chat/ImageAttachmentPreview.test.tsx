import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { ImageAttachmentPreview } from './ImageAttachmentPreview'
import { PersistedMessageContent } from './MessageBlocks'
import { sanitizeAttachment } from '../../store/cpp/sanitizers'
import { buildMessageFromCpp, attachmentsEquivalent, reconcileCppMessages } from '../../store/cpp/reconcile'
import type { Attachment, Message } from '../../types/message'
import type { CppMessage } from '../../store/cpp/types'

const request = vi.hoisted(() => vi.fn())
vi.mock('../../ipc/cefBridge', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../ipc/cefBridge')>()),
  sendToCEF: request,
}))

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
let visible: (entries: Pick<IntersectionObserverEntry, 'isIntersecting'>[]) => void
let host: HTMLDivElement
let root: ReturnType<typeof createRoot>
const attachment: Attachment = {
  id: 'owned-image', name: 'photo.png', type: 'file', mimeType: 'image/png', size: 100, path: '/untrusted/path',
}
const validPreview = { mimeType: 'image/png', dataBase64: 'aGk=', width: 2, height: 1 }

function render(index = 7, chatId = 'source-chat', value = attachment) {
  act(() => root.render(<ImageAttachmentPreview chatId={chatId} messageIndex={index} attachment={value}
    fallback={<span>original-file-entry</span>} />))
}

async function reveal() {
  await act(async () => visible([{ isIntersecting: true }]))
}

beforeEach(() => {
  request.mockReset()
  host = document.createElement('div')
  document.body.append(host)
  root = createRoot(host)
  vi.stubGlobal('IntersectionObserver', class {
    constructor(callback: typeof visible) { visible = callback }
    observe() {}
    disconnect() {}
  })
  Object.defineProperty(URL, 'createObjectURL', { configurable: true, value: vi.fn(() => 'blob:private-preview') })
  Object.defineProperty(URL, 'revokeObjectURL', { configurable: true, value: vi.fn() })
})

afterEach(() => {
  act(() => root.unmount())
  host.remove()
  vi.unstubAllGlobals()
})

it('requests only after visibility, scopes by native index and revokes on unmount', async () => {
  request.mockResolvedValue({ ok: true, data: validPreview })
  render()
  expect(request).not.toHaveBeenCalled()
  expect(host.textContent).toBe('original-file-entry')
  await reveal()
  expect(request).toHaveBeenCalledExactlyOnceWith({
    action: 'getAttachmentImagePreview',
    payload: { chatId: 'source-chat', messageIndex: 7, attachmentId: 'owned-image' },
  }, false)
  expect(host.querySelector('img')?.getAttribute('src')).toBe('blob:private-preview')
  act(() => root.unmount())
  expect(URL.revokeObjectURL).toHaveBeenCalledWith('blob:private-preview')
  root = createRoot(host)
})

it('never derives an index from a legacy renderer id', () => {
  act(() => root.render(<ImageAttachmentPreview chatId="cef-m-chat-7" attachment={attachment} fallback="file" />))
  expect(request).not.toHaveBeenCalled()
  expect(host.textContent).toBe('file')
})

it.each([
  { ...validPreview, mimeType: 'image/svg+xml' },
  { ...validPreview, dataBase64: '!!!!' },
  { ...validPreview, dataBase64: btoa('x'.repeat(524289)) },
  { ...validPreview, height: 0 },
  { ...validPreview, width: 1025 },
  { ...validPreview, height: 1025 },
])('retains the ordinary file for invalid or oversized response %#', async (data) => {
  request.mockResolvedValue({ ok: true, data })
  render()
  await reveal()
  expect(host.textContent).toBe('original-file-entry')
  expect(URL.createObjectURL).not.toHaveBeenCalled()
})

it('ignores a stale response after changing message scope', async () => {
  let resolve!: (value: unknown) => void
  request.mockImplementationOnce(() => new Promise((complete) => { resolve = complete }))
  render()
  act(() => visible([{ isIntersecting: true }]))
  render(8, 'other-chat')
  await act(async () => resolve({ ok: true, data: validPreview }))
  expect(URL.createObjectURL).not.toHaveBeenCalled()
  expect(host.textContent).toBe('original-file-entry')
})

it('revokes a replaced attachment even when its id and MIME are reused', async () => {
  request.mockResolvedValue({ ok: true, data: validPreview })
  render()
  await reveal()
  render(7, 'source-chat', { ...attachment, path: '/replacement/path', name: 'replacement.png', size: 200 })
  expect(URL.revokeObjectURL).toHaveBeenCalledWith('blob:private-preview')
  expect(host.querySelector('img')).toBeNull()
  await reveal()
  expect(request).toHaveBeenCalledTimes(2)
  expect(host.querySelector('img')?.getAttribute('alt')).toBe('replacement.png')
})

it('revokes an image that cannot be decoded and restores the file entry', async () => {
  request.mockResolvedValue({ ok: true, data: validPreview })
  render()
  await reveal()
  act(() => host.querySelector('img')!.dispatchEvent(new Event('error')))
  expect(URL.revokeObjectURL).toHaveBeenCalledWith('blob:private-preview')
  expect(host.querySelector('img')).toBeNull()
  expect(host.textContent).toBe('original-file-entry')
})

it('maps the absolute native index and retains MIME independently of file kind', () => {
  const raw = { id: 'a', name: 'a.png', kind: 'file', type: 'image/png', size: 2 }
  expect(sanitizeAttachment(raw)).toMatchObject({ type: 'file', mimeType: 'image/png' })
  const message = buildMessageFromCpp('chat', { role: 'user', content: '', createdAt: '2026-01-01', attachments: [raw] }, 42)
  expect(message.nativeMessageIndex).toBe(42)
  expect(message.attachments?.[0]).toMatchObject({ type: 'file', mimeType: 'image/png' })
  expect(attachmentsEquivalent([attachment], [{ ...attachment, mimeType: 'image/jpeg' }])).toBe(false)
})

it('uses the explicit source chat in shared normal and side persisted rendering', async () => {
  request.mockResolvedValue({ ok: false, error: 'unavailable' })
  const message: Message = {
    id: 'renderer-only-id', sessionId: 'display-chat', role: 'user', content: '', createdAt: new Date(),
    nativeMessageIndex: 23, attachments: [attachment],
  }
  act(() => root.render(<PersistedMessageContent message={message} sourceChatId="source-chat" onSelectTool={() => undefined} />))
  await reveal()
  expect(request).toHaveBeenCalledExactlyOnceWith({
    action: 'getAttachmentImagePreview',
    payload: { chatId: 'source-chat', messageIndex: 23, attachmentId: 'owned-image' },
  }, false)
  expect(host.querySelector('img')).toBeNull()
  expect(host.textContent).toContain('/untrusted/path')
})

it('replaces stale renderer indices instead of retaining shifted preview identity', () => {
  const raw: CppMessage = { role: 'user', content: 'same', createdAt: '2026-01-01' }
  const old = buildMessageFromCpp('chat', raw, 42)
  const next = reconcileCppMessages('chat', [old], [raw], true)
  expect(next[0]).not.toBe(old)
  expect(next[0].nativeMessageIndex).toBe(0)
})

const previewMetadataChanges = [
  ['previewKey', 'a'.repeat(64), 'b'.repeat(64)],
  ['previewOriginPath', '/original/photo.png', '/replacement/photo.png'],
] as const

it.each(previewMetadataChanges)('revokes and reloads when only %s changes', async (field, initial, replacement) => {
  request.mockResolvedValue({ ok: true, data: validPreview })
  render(7, 'source-chat', { ...attachment, [field]: initial })
  await reveal()
  render(7, 'source-chat', { ...attachment, [field]: replacement })
  expect(URL.revokeObjectURL).toHaveBeenCalledWith('blob:private-preview')
  expect(host.querySelector('img')).toBeNull()
  await reveal()
  expect(request).toHaveBeenCalledTimes(2)
  expect(request.mock.calls[1]).toEqual([{
    action: 'getAttachmentImagePreview',
    payload: { chatId: 'source-chat', messageIndex: 7, attachmentId: 'owned-image' },
  }, false])
})

it.each(previewMetadataChanges)('ignores an old response when only %s changes', async (field, initial, replacement) => {
  let resolve!: (value: unknown) => void
  request.mockImplementationOnce(() => new Promise((complete) => { resolve = complete }))
  render(7, 'source-chat', { ...attachment, [field]: initial })
  act(() => visible([{ isIntersecting: true }]))
  render(7, 'source-chat', { ...attachment, [field]: replacement })
  await act(async () => resolve({ ok: true, data: validPreview }))
  expect(URL.createObjectURL).not.toHaveBeenCalled()
  expect(host.textContent).toBe('original-file-entry')
})

it.each(previewMetadataChanges)('reconciles a native attachment when only %s changes', (field, initial, replacement) => {
  const raw: CppMessage = {
    role: 'user', content: 'same', createdAt: '2026-01-01',
    attachments: [{ ...attachment, [field]: initial }],
  }
  const old = buildMessageFromCpp('chat', raw, 0)
  expect(old.attachments?.[0][field]).toBe(initial)
  expect(attachmentsEquivalent(old.attachments ?? [], [{ ...attachment, [field]: replacement }])).toBe(false)
  const next = reconcileCppMessages('chat', [old], [{ ...raw, attachments: [{ ...attachment, [field]: replacement }] }], true)
  expect(next[0]).not.toBe(old)
  expect(next[0].attachments?.[0][field]).toBe(replacement)
})
