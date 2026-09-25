import { describe, expect, it } from 'vitest'
import { isRemoteDirectoryBrowseAvailable } from './remoteWorkspace'

describe('remote workspace browse eligibility', () => {
  it('allows rechecking installed helpers after a health failure', () => {
    expect(isRemoteDirectoryBrowseAvailable({ runnerStatus: 'ready', runnerVersion: '1.0.0' })).toBe(true)
    expect(isRemoteDirectoryBrowseAvailable({ runnerStatus: 'error', runnerVersion: '1.0.0' })).toBe(true)
    expect(isRemoteDirectoryBrowseAvailable({ runnerStatus: 'offline', runnerVersion: '1.0.0' })).toBe(true)
  })

  it('does not admit uninstalled or in progress helpers', () => {
    expect(isRemoteDirectoryBrowseAvailable({ runnerStatus: 'uninstalled', runnerVersion: '' })).toBe(false)
    expect(isRemoteDirectoryBrowseAvailable({ runnerStatus: 'installing', runnerVersion: '1.0.0' })).toBe(false)
    expect(isRemoteDirectoryBrowseAvailable({ runnerStatus: 'error', runnerVersion: '' })).toBe(false)
  })
})
