export type CustomIconTarget = 'workspace' | 'host' | 'collection' | 'desktop-app'
export type CustomIconInput = { type: 'text'; value: string } | { type: 'png'; base64: string } | null

export type CustomIcon = { type: 'text'; value: string } | { type: 'png'; value: string; dataUrl?: string }

export function isSingleIconText(value: string): boolean {
  const Segmenter = (Intl as unknown as { Segmenter?: new (locale?: string, options?: { granularity: 'grapheme' }) => { segment: (text: string) => Iterable<unknown> } }).Segmenter
  return (Segmenter ? [...new Segmenter(undefined, { granularity: 'grapheme' }).segment(value)] : Array.from(value)).length === 1
}

/** Accepts portable metadata and bounded image data supplied by the native asset store. */
export function sanitizeCustomIcon(value: unknown): CustomIcon | undefined {
  if (!value || typeof value !== 'object') return undefined
  const icon = value as Record<string, unknown>
  if (typeof icon.value !== 'string') return undefined
  if (icon.type === 'text' && isSingleIconText(icon.value) && new TextEncoder().encode(icon.value).length <= 64 && !/[\x00-\x1f\x7f]/.test(icon.value)) {
    return { type: 'text', value: icon.value }
  }
  if (icon.type === 'png' && /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\.png$/i.test(icon.value)) {
    const dataUrl = typeof icon.dataUrl === 'string' && icon.dataUrl.length <= 350000 && /^data:image\/png;base64,[A-Za-z0-9+/]+={0,2}$/.test(icon.dataUrl) ? icon.dataUrl : undefined
    return { type: 'png', value: icon.value, dataUrl }
  }
  return undefined
}

/** Workspace overrides host; unavailable images fall through to the next usable icon. */
export function resolveCustomIcon(workspace?: CustomIcon, host?: CustomIcon, failedAssets: ReadonlySet<string> = new Set()): CustomIcon | undefined {
  return [workspace, host].find((icon) => icon && (icon.type === 'text' || Boolean(icon.dataUrl && !failedAssets.has(icon.value))))
}

export function customIconsEqual(left?: CustomIcon, right?: CustomIcon): boolean {
  return left?.type === right?.type && left?.value === right?.value &&
    (left?.type === 'png' ? left.dataUrl : undefined) === (right?.type === 'png' ? right.dataUrl : undefined)
}
