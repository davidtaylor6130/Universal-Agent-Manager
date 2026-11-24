import { describe, expect, it } from 'vitest'
import { sanitizeCppStatePatch } from './sanitizers'

const action = {
  id: 'review', label: 'Review', skillPath: '', providerId: '', modelId: '', groupPath: ['GitHub'],
  acceptsFiles: true, acceptsFolders: true, enabled: true, openWorkspace: true,
}

describe('shell actions transport', () => {
  it('keeps the reason a saved entry is invalid and leaves valid entries without one', () => {
    const kept = { ...action, id: 'kept', label: 'Kept', enabled: false, invalidReason: " Its saved 'enabled' value has the wrong type. " }
    const patch = sanitizeCppStatePatch({ stateRevision: 2, shellActions: [action, kept] })!
    expect(patch.shellActions).toEqual([action, { ...kept, invalidReason: "Its saved 'enabled' value has the wrong type." }])
    expect(patch.shellActions?.[0]).not.toHaveProperty('invalidReason')
    expect(sanitizeCppStatePatch({ stateRevision: 3, shellActions: [{ ...action, invalidReason: 7 }] })!.shellActions?.[0]).not.toHaveProperty('invalidReason')
  })
})
