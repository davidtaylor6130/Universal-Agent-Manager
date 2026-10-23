import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { ImageAttachmentPreview } from './ImageAttachmentPreview'
import { PersistedMessageContent } from './MessageBlocks'
import { sanitizeAttachment, sanitizeCppMessage } from '../../store/cpp/sanitizers'
import { FileInspectionContext } from '../shared/fileInspectionContext'
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
  expect(host.querySelector('img')?.className).toBe('conversation-image-preview h-20 w-20 object-contain')
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
  expect(host.textContent).toContain('photo.png')
  expect(host.querySelector('[data-detail="/untrusted/path"]')).not.toBeNull()
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

const nativeMetadata = {
  source: 'owned_native_asset' as const, originExecutionHostId: 'ssh-origin',
  nativePartKey: 'native-message/content/0', nativeToolCallId: 'native-tool', nativeSourceField: 'content',
  nativeAssetKey: 'private-owner/generation/original', contentDigest: 'a'.repeat(64),
}

it.each(['owned_native_asset', 'provider_uri', 'native_inline_pending'] as const)('retains pathless %s metadata through saved attachment ingress without projecting payload', source => {
  const raw = { id: 'native', name: 'Recorded original', kind: 'file', mimeType: 'application/pdf', sizeBytes: 0,
    ...nativeMetadata, source, sourceUri: 'file:///opaque%20reference.pdf', path: '/must-not-become-authority',
    previewOriginPath: '/must-not-open', dataBase64: 'PRIVATE-BYTES', nativeInlinePayload: { encoded: 'PRIVATE-BYTES' } }
  const sanitized = sanitizeAttachment(raw)!
  expect(sanitized).toMatchObject({ ...nativeMetadata, source, sourceUri: raw.sourceUri })
  expect(sanitized.path).toBeUndefined()
  expect(sanitized.previewOriginPath).toBeUndefined()
  expect(JSON.stringify(sanitized)).not.toContain('PRIVATE-BYTES')
  const message = sanitizeCppMessage({ role: 'user', content: '', createdAt: '2026-01-01', attachments: [raw] })!
  const built = buildMessageFromCpp('native-chat', message, 13)
  expect(built.attachments).toHaveLength(1)
  expect(built.attachments?.[0]).toMatchObject({ ...nativeMetadata, source, sourceUri: raw.sourceUri })
})

const nativeMetadataChanges = [
  ['source', 'owned_native_asset', 'provider_uri'],
  ['sourceUri', 'gs://bucket/original', 'https://opaque.invalid/replacement'],
  ['originExecutionHostId', 'ssh-one', 'ssh-two'],
  ['nativePartKey', 'part-one', 'part-two'],
  ['nativeToolCallId', 'tool-one', 'tool-two'],
  ['nativeSourceField', 'content', 'displayContent'],
  ['nativeAssetKey', 'asset-one', 'asset-two'],
  ['contentDigest', 'a'.repeat(64), 'b'.repeat(64)],
] as const

it.each(nativeMetadataChanges)('adopts metadata-only native history updates to %s', (field, initial, replacement) => {
  const value = { ...attachment, ...nativeMetadata, path: undefined, [field]: initial }
  const raw: CppMessage = { role: 'user', content: '', createdAt: '2026-01-01', attachments: [value] }
  const before = buildMessageFromCpp('chat', raw, 0)
  const changed = { ...value, [field]: replacement }
  expect(attachmentsEquivalent(before.attachments ?? [], [changed])).toBe(false)
  const after = reconcileCppMessages('chat', [before], [{ ...raw, attachments: [changed] }], true)
  expect(after[0]).not.toBe(before)
  expect(after[0].attachments?.[0]).toMatchObject({ [field]: replacement })
  expect(reconcileCppMessages('chat', after, [{ ...raw, attachments: [changed] }], true)).toBe(after)
})

it.each(['file:///private/picture.png', 'https://opaque.invalid/picture.png', 'gs://bucket/picture', 'data:image/png;base64,opaque'])('keeps provider URI %s plain and unavailable even with an image MIME and misleading path', async sourceUri => {
  vi.stubGlobal('IntersectionObserver', undefined)
  const inspect = vi.fn()
  const value = { ...attachment, ...nativeMetadata, source: 'provider_uri' as const, sourceUri, size: 0 }
  const message: Message = { id: 'uri-only', sessionId: 'chat', role: 'user', content: '', nativeMessageIndex: 8, createdAt: new Date(0), attachments: [value] }
  await act(async () => root.render(<FileInspectionContext.Provider value={inspect}>
    <PersistedMessageContent message={message} sourceChatId="source-chat" onSelectTool={() => undefined} />
  </FileInspectionContext.Provider>))
  expect(host.textContent).toContain('External reference')
  expect(host.textContent).toContain('Bytes unavailable')
  expect(host.textContent).not.toMatch(/0\s*(bytes|B)\b/)
  expect(host.querySelector('[data-detail]')?.getAttribute('data-detail')).toContain(sourceUri)
  expect(host.querySelector('a, img, button.uam-context-chip__body')).toBeNull()
  const chip = host.querySelector<HTMLElement>('.uam-context-chip__body')!
  act(() => chip.click())
  expect(inspect).not.toHaveBeenCalled()
  expect(request).not.toHaveBeenCalled()
  expect(URL.createObjectURL).not.toHaveBeenCalled()
})

it.each(['application/pdf', 'audio/mpeg', 'video/mp4'])('shows recorded %s originals without claiming preview or playback', async mimeType => {
  const message: Message = { id: 'media-only', sessionId: 'chat', role: 'assistant', content: '', nativeMessageIndex: 9, createdAt: new Date(0),
    attachments: [{ ...attachment, ...nativeMetadata, path: undefined, mimeType }] }
  await act(async () => root.render(<PersistedMessageContent message={message} onSelectTool={() => undefined} />))
  expect(host.textContent).toContain('Recorded file')
  expect(host.textContent).toContain('Preview unavailable')
  expect(host.querySelector('img, audio, video, a')).toBeNull()
  expect(request).not.toHaveBeenCalled()
})

it('keeps a pending original visible and unavailable without requesting its bytes', async () => {
  vi.stubGlobal('IntersectionObserver', undefined)
  const message: Message = { id: 'pending-only', sessionId: 'chat', role: 'user', content: '', nativeMessageIndex: 9, createdAt: new Date(0),
    attachments: [{ ...attachment, ...nativeMetadata, source: 'native_inline_pending', path: undefined }] }
  await act(async () => root.render(<PersistedMessageContent message={message} onSelectTool={() => undefined} />))
  expect(host.textContent).toContain('Recorded file')
  expect(host.textContent).toContain('Bytes unavailable')
  expect(request).not.toHaveBeenCalled()
})

it('requests a remote-origin owned original only by stored row identity and reports an unavailable preview', async () => {
  request.mockResolvedValue({ ok: true, data: { available: false } })
  const value = { ...attachment, ...nativeMetadata, path: undefined }
  const message: Message = { id: 'owned-only', sessionId: 'chat', role: 'user', content: '', nativeMessageIndex: 12, createdAt: new Date(0), attachments: [value] }
  act(() => root.render(<PersistedMessageContent message={message} sourceChatId="real-source" onSelectTool={() => undefined} />))
  expect(request).not.toHaveBeenCalled()
  await reveal()
  expect(request).toHaveBeenCalledExactlyOnceWith({ action: 'getAttachmentImagePreview', payload: {
    chatId: 'real-source', messageIndex: 12, attachmentId: value.id,
  } }, false)
  expect(host.textContent).toContain('Recorded file')
  expect(host.textContent).toContain('Preview unavailable')
  expect(host.querySelector('img')).toBeNull()
})

it.each(['nativeAssetKey', 'contentDigest', 'originExecutionHostId', 'nativePartKey', 'nativeToolCallId', 'nativeSourceField', 'sourceUri'] as const)('disposes an owned preview when only %s changes', async field => {
  let complete!: (value: unknown) => void
  request.mockImplementationOnce(() => new Promise(resolve => { complete = resolve }))
  const value = { ...attachment, ...nativeMetadata, path: undefined }
  render(7, 'source-chat', value)
  act(() => visible([{ isIntersecting: true }]))
  render(7, 'source-chat', { ...value, [field]: 'replacement' })
  await act(async () => complete({ ok: true, data: validPreview }))
  expect(URL.createObjectURL).not.toHaveBeenCalled()
  expect(host.querySelector('img')).toBeNull()
})

it('previews a pathless SSH-origin owned image through the exact stored row, and revokes it when it becomes an opaque reference', async () => {
  request.mockResolvedValue({ ok: true, data: validPreview })
  const value = { ...attachment, ...nativeMetadata, path: undefined }
  render(21, 'passive-child', value)
  await reveal()
  expect(request).toHaveBeenCalledExactlyOnceWith({ action: 'getAttachmentImagePreview', payload: {
    chatId: 'passive-child', messageIndex: 21, attachmentId: value.id,
  } }, false)
  expect(host.querySelector('img')?.getAttribute('src')).toBe('blob:private-preview')
  render(21, 'passive-child', { ...value, source: 'provider_uri', sourceUri: 'https://opaque.invalid/picture.png' })
  expect(URL.revokeObjectURL).toHaveBeenCalledWith('blob:private-preview')
  expect(host.querySelector('img')).toBeNull()
  expect(request).toHaveBeenCalledTimes(1)
})

it('keeps a recorded image unavailable when its stored native message index is missing', async () => {
  const message: Message = { id: 'renderer-id-is-not-native-index-21', sessionId: 'chat', role: 'user', content: '', createdAt: new Date(0),
    attachments: [{ ...attachment, ...nativeMetadata, path: undefined }] }
  await act(async () => root.render(<PersistedMessageContent message={message} onSelectTool={() => undefined} />))
  expect(host.textContent).toContain('Recorded file')
  expect(host.textContent).toContain('Preview unavailable')
  expect(request).not.toHaveBeenCalled()
})

it('validates native metadata strings without changing legacy path semantics or admitting payload fields', () => {
  const raw = { ...attachment, source: 'legacy_path', sourceUri: 42, originExecutionHostId: false, nativePartKey: {},
    nativeToolCallId: [], nativeSourceField: null, nativeAssetKey: false, contentDigest: 42, dataBase64: 'PRIVATE-BYTES' }
  expect(sanitizeAttachment(raw)).toEqual({ ...attachment, source: 'legacy_path' })
  expect(attachmentsEquivalent([attachment], [{ ...attachment, source: 'legacy_path' }])).toBe(true)
  const native = sanitizeAttachment({ ...raw, ...nativeMetadata, source: 'provider_uri', sourceUri: 'gs://opaque/exact%20bytes', previewKey: 'a'.repeat(64) })!
  expect(native.sourceUri).toBe('gs://opaque/exact%20bytes')
  expect(native.path).toBeUndefined()
  expect(native.previewKey).toBeUndefined()
})

it('NM-R2-01 preserves a visible legacy preview across direct absent and explicit legacy source props', async () => {
  const raw: CppMessage = { role: 'user', content: '', createdAt: '2026-01-01', attachments: [attachment] }
  const stored = buildMessageFromCpp('source-chat', raw, 0)
  const retained = reconcileCppMessages('source-chat', [stored], [{ ...raw, attachments: [{ ...attachment, source: 'legacy_path' }] }], true)
  expect(retained[0]).toBe(stored)
  expect(retained[0].attachments?.[0].source).toBeUndefined()

  request.mockResolvedValue({ ok: true, data: validPreview })
  render()
  await reveal()
  const image = host.querySelector('img')
  expect(image).not.toBeNull()
  render(7, 'source-chat', { ...attachment, source: 'legacy_path' })
  expect(URL.revokeObjectURL).not.toHaveBeenCalled()
  expect(host.querySelector('img')).toBe(image)
  await reveal()
  render(7, 'source-chat', attachment)
  await reveal()
  expect(host.querySelector('img')).toBe(image)
  expect(request).toHaveBeenCalledTimes(1)
  expect(URL.createObjectURL).toHaveBeenCalledTimes(1)
})

it('NM-R2-02 invalidates an owned previewKey and refetches changed cached content through the same saved-row triple', async () => {
  const value = sanitizeAttachment({ ...attachment, ...nativeMetadata, path: undefined, previewKey: 'a'.repeat(64) })!
  expect(value.previewKey).toBe('a'.repeat(64))
  vi.mocked(URL.createObjectURL).mockReturnValueOnce('blob:first-cache').mockReturnValueOnce('blob:second-cache')
  request.mockResolvedValueOnce({ ok: true, data: validPreview })
    .mockResolvedValueOnce({ ok: true, data: { ...validPreview, dataBase64: 'bmV3', width: 3 } })
  render(17, 'stored-owner', value)
  await reveal()
  expect(host.querySelector('img')?.getAttribute('src')).toBe('blob:first-cache')
  render(17, 'stored-owner', { ...value, previewKey: 'b'.repeat(64) })
  expect(URL.revokeObjectURL).toHaveBeenCalledWith('blob:first-cache')
  expect(host.querySelector('img')).toBeNull()
  await reveal()
  expect(host.querySelector('img')?.getAttribute('src')).toBe('blob:second-cache')
  expect(host.querySelector('img')?.getAttribute('width')).toBe('3')
  const exactRequest = [{ action: 'getAttachmentImagePreview', payload: {
    chatId: 'stored-owner', messageIndex: 17, attachmentId: value.id,
  } }, false]
  expect(request.mock.calls).toEqual([exactRequest, exactRequest])
})
