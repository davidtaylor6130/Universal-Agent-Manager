import { useEffect, useRef, useState, type ReactNode } from 'react'
import { sendToCEF } from '../../ipc/cefBridge'
import type { Attachment } from '../../types/message'

const MAX_PREVIEW_BYTES = 512 * 1024
const MAX_BASE64_LENGTH = 4 * Math.ceil(MAX_PREVIEW_BYTES / 3)

type Preview = { url: string; width: number; height: number; identity: string }

function previewBlob(value: unknown): { blob: Blob; width: number; height: number } | null {
  if (!value || typeof value !== 'object') return null
  const data = value as Record<string, unknown>
  if (data.mimeType !== 'image/png' && data.mimeType !== 'image/jpeg') return null
  if (!Number.isSafeInteger(data.width) || !Number.isSafeInteger(data.height) || Number(data.width) < 1 || Number(data.height) < 1 || Number(data.width) > 1024 || Number(data.height) > 1024) return null
  if (typeof data.dataBase64 !== 'string' || !data.dataBase64 || data.dataBase64.length > MAX_BASE64_LENGTH || data.dataBase64.length % 4 !== 0) return null
  if (!/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(data.dataBase64)) return null
  try {
    const decoded = atob(data.dataBase64)
    if (!decoded.length || decoded.length > MAX_PREVIEW_BYTES || btoa(decoded) !== data.dataBase64) return null
    const bytes = Uint8Array.from(decoded, (char) => char.charCodeAt(0))
    return { blob: new Blob([bytes], { type: data.mimeType }), width: Number(data.width), height: Number(data.height) }
  } catch {
    return null
  }
}

/** Requests a bounded native thumbnail only when this persisted attachment becomes visible. */
export function ImageAttachmentPreview({ chatId, messageIndex, attachment, fallback, unavailableFallback = fallback }: {
  chatId?: string
  messageIndex?: number
  attachment: Attachment
  fallback: ReactNode
  unavailableFallback?: ReactNode
}) {
  const host = useRef<HTMLSpanElement>(null)
  const [preview, setPreview] = useState<Preview | null>(null)
  const [unavailableIdentity, setUnavailableIdentity] = useState<string | null>(null)
  const identity = JSON.stringify([chatId, messageIndex, attachment.id, attachment.mimeType, attachment.path, attachment.name, attachment.size, attachment.previewKey, attachment.previewOriginPath,
    attachment.source ?? 'legacy_path', attachment.sourceUri, attachment.originExecutionHostId, attachment.nativePartKey, attachment.nativeToolCallId, attachment.nativeSourceField, attachment.nativeAssetKey, attachment.contentDigest])
  const sourceSupported = attachment.source === 'owned_native_asset' || ((attachment.source === undefined || attachment.source === 'legacy_path') && !attachment.sourceUri)
  const eligible = sourceSupported && Boolean(chatId && attachment.id) && Number.isSafeInteger(messageIndex) && Number(messageIndex) >= 0 &&
    (attachment.mimeType === 'image/png' || attachment.mimeType === 'image/jpeg')

  useEffect(() => {
    setPreview(null)
    if (!eligible) return
    let disposed = false
    let requested = false
    let url: string | undefined
    let observer: IntersectionObserver | undefined
    const request = async () => {
      if (disposed || requested) return
      requested = true
      observer?.disconnect()
      try {
        const response = await sendToCEF<unknown>({ action: 'getAttachmentImagePreview', payload: {
          chatId, messageIndex, attachmentId: attachment.id,
        } }, false)
        if (disposed) return
        if (!response.ok) { setUnavailableIdentity(identity); return }
        const image = previewBlob(response.data)
        if (!image) { setUnavailableIdentity(identity); return }
        if (disposed) return
        url = URL.createObjectURL(image.blob)
        setPreview({ url, width: image.width, height: image.height, identity })
      } catch {
        // Unavailable images retain their ordinary attachment entry.
        if (!disposed) setUnavailableIdentity(identity)
      }
    }
    if (typeof IntersectionObserver === 'undefined') void request()
    else if (host.current) {
      observer = new IntersectionObserver((entries) => {
        if (entries.some((entry) => entry.isIntersecting)) void request()
      })
      observer.observe(host.current)
    }
    return () => {
      disposed = true
      observer?.disconnect()
      if (url) URL.revokeObjectURL(url)
    }
  }, [chatId, messageIndex, attachment.id, identity, eligible])

  if (!eligible) return <>{unavailableFallback}</>

  const current = preview?.identity === identity ? preview : null
  return <span ref={host} className="inline-flex max-w-full">
    {current ? <img src={current.url} alt={attachment.name} width={current.width} height={current.height}
      className="conversation-image-preview h-20 w-20 object-contain" onError={() => { URL.revokeObjectURL(current.url); setPreview(null); setUnavailableIdentity(identity) }} /> : unavailableIdentity === identity ? unavailableFallback : fallback}
  </span>
}
