import { describe, expect, it } from 'vitest'
import { resolveCustomIcon, sanitizeCustomIcon, type CustomIcon } from './customIcon'

describe('portable custom icons', () => {
  it('accepts emoji and rejects unsafe or unbounded metadata', () => {
    expect(sanitizeCustomIcon({ type: 'text', value: '🛠️' })).toEqual({ type: 'text', value: '🛠️' })
    expect(sanitizeCustomIcon({ type: 'text', value: '\n' })).toBeUndefined()
    expect(sanitizeCustomIcon({ type: 'text', value: 'AB' })).toBeUndefined()
    expect(sanitizeCustomIcon({ type: 'text', value: '🚀'.repeat(17) })).toBeUndefined()
    expect(sanitizeCustomIcon({ type: 'png', value: '../secret.png' })).toBeUndefined()
    expect(sanitizeCustomIcon({ type: 'svg', value: '<svg />' })).toBeUndefined()
  })
  it('keeps portable PNG references while discarding arbitrary URLs', () => {
    const value = '12345678-1234-1234-1234-123456789abc.png'
    expect(sanitizeCustomIcon({ type: 'png', value, dataUrl: 'file:///tmp/icon.png' })).toEqual({ type: 'png', value, dataUrl: undefined })
    expect(sanitizeCustomIcon({ type: 'png', value, dataUrl: 'data:image/png;base64,YQ==' })).toEqual({ type: 'png', value, dataUrl: 'data:image/png;base64,YQ==' })
  })
  it('gives workspace precedence and falls back after missing or failed images', () => {
    const host: CustomIcon = { type: 'text', value: 'H' }
    const workspace: CustomIcon = { type: 'png', value: 'image.png', dataUrl: 'data:image/png;base64,YQ==' }
    expect(resolveCustomIcon(workspace, host)).toBe(workspace)
    expect(resolveCustomIcon(workspace, host, new Set(['image.png']))).toBe(host)
    expect(resolveCustomIcon({ type: 'png', value: 'missing.png' }, host)).toBe(host)
    expect(resolveCustomIcon({ type: 'png', value: 'missing.png' })).toBeUndefined()
  })
})
