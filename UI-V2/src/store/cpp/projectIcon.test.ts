import { describe, expect, it } from 'vitest'
import { sanitizeCppFolder } from './sanitizers'

const png = 'data:image/png;base64,iVBORw0KGgo='

describe('detected workspace project icon', () => {
  it('shows the project icon only when no custom icon is chosen', () => {
    expect(sanitizeCppFolder({ id: 'f1', projectIcon: png })?.customIcon).toEqual({ type: 'png', value: 'project-icon:f1', dataUrl: png })
    expect(sanitizeCppFolder({ id: 'f1', projectIcon: png, customIcon: { type: 'text', value: 'A' } })?.customIcon).toEqual({ type: 'text', value: 'A' })
  })

  it('rejects anything that is not PNG data', () => {
    expect(sanitizeCppFolder({ id: 'f1', projectIcon: 'data:image/svg+xml;base64,PHN2Zz4=' })?.customIcon).toBeUndefined()
    expect(sanitizeCppFolder({ id: 'f1', projectIcon: 'file:///etc/passwd' })?.customIcon).toBeUndefined()
  })
})
