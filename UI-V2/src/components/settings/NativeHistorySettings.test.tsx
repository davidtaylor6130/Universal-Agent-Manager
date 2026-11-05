import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { NativeHistorySettings, NativeImportFilters } from './NativeHistorySettings'
import { useAppStore } from '../../store/useAppStore'
import { EMPTY_STARTUP_NATIVE_IMPORT_STATUS, type StartupNativeImportPage } from '../../types/startupNativeImports'
import { SUPPORTED_CLI_PROVIDER_IDS, fallbackProviderForId } from '../../utils/providerMetadata'
import { APP_FAILURE_EVENT, type AppFailure } from '../../utils/appFailure'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const page = (phase: StartupNativeImportPage['phase'] = 'scanning', generation = 'explicit-1'): StartupNativeImportPage => ({
  phase, generation, totalCount: 0, importedCount: 0, candidates: [], nextOffset: 0, errors: [],
})
let host: HTMLDivElement
let root: ReturnType<typeof createRoot>
let requests: { action: string; payload: Record<string, unknown>; success: (value: string) => void; failure: (code: number, message: string) => void }[]
const button = (label: string) => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((item) => item.textContent === label)!
const answer = async (index: number, data: StartupNativeImportPage) => { await act(async () => requests[index].success(JSON.stringify(data))) }

const failures: AppFailure[] = []
const collectFailure = (event: Event) => { failures.push((event as CustomEvent<AppFailure>).detail) }
beforeEach(() => { failures.length = 0; window.addEventListener(APP_FAILURE_EVENT, collectFailure) })
afterEach(() => window.removeEventListener(APP_FAILURE_EVENT, collectFailure))

describe('Chat Data import filters', () => {
  it('saves cleaned rules, enables Save only when changed and keeps the draft when saving fails', async () => {
    const save = vi.fn().mockResolvedValueOnce(false).mockResolvedValueOnce(true)
    useAppStore.setState({ nativeImportExcludedTitlePrefixes: ['[automation]'], nativeImportExcludedDirectories: [], nativeImportStartupMaxAgeDays: 0, setNativeImportFilters: save })
    const filterHost = document.createElement('div'); document.body.append(filterHost)
    const filterRoot = createRoot(filterHost)
    try {
      act(() => filterRoot.render(<NativeImportFilters />))
      const saveButton = () => Array.from(filterHost.querySelectorAll<HTMLButtonElement>('button')).find((item) => item.textContent === 'Save filters')!
      const type = (id: string, value: string) => act(() => {
        const field = filterHost.querySelector<HTMLTextAreaElement>(`#${id}`)!
        Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(field, value)
        field.dispatchEvent(new Event('input', { bubbles: true }))
      })
      expect(filterHost.querySelector<HTMLTextAreaElement>('#native-import-title-prefixes')!.value).toBe('[automation]')
      expect(saveButton().disabled).toBe(true)
      type('native-import-title-prefixes', '[automation]\n [AUTOMATION] ')
      expect(saveButton().disabled).toBe(true)
      type('native-import-directories', ' C:\\scratch \n\nC:\\scratch\n/home/you/tmp')
      expect(saveButton().disabled).toBe(false)
      await act(async () => saveButton().click())
      expect(save).toHaveBeenLastCalledWith({ titlePrefixes: ['[automation]'], directories: ['C:\\scratch', '/home/you/tmp'], startupMaxAgeDays: 0 })
      expect(failures.at(-1)).toMatchObject({ message: 'Import filters could not be saved.', detail: expect.stringContaining('Your changes are kept') })
      expect(filterHost.querySelector<HTMLTextAreaElement>('#native-import-directories')!.value).toContain('/home/you/tmp')
      await act(async () => saveButton().click())
      expect(save).toHaveBeenCalledTimes(2)
    } finally { act(() => filterRoot.unmount()); filterHost.remove() }
  })

  it('saves the startup age limit and blocks an invalid one', async () => {
    const save = vi.fn().mockResolvedValue(true)
    useAppStore.setState({ nativeImportExcludedTitlePrefixes: [], nativeImportExcludedDirectories: [], nativeImportStartupMaxAgeDays: 30, setNativeImportFilters: save })
    const filterHost = document.createElement('div'); document.body.append(filterHost)
    const filterRoot = createRoot(filterHost)
    try {
      act(() => filterRoot.render(<NativeImportFilters />))
      const saveButton = () => Array.from(filterHost.querySelectorAll<HTMLButtonElement>('button')).find((item) => item.textContent === 'Save filters')!
      const field = filterHost.querySelector<HTMLInputElement>('#native-import-startup-age')!
      const type = (value: string) => act(() => {
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(field, value)
        field.dispatchEvent(new Event('input', { bubbles: true }))
      })
      expect(field.value).toBe('30')
      expect(saveButton().disabled).toBe(true)
      type('4000')
      expect(saveButton().disabled).toBe(true)
      expect(filterHost.textContent).toContain('from 0 to 3650')
      expect(field.getAttribute('aria-describedby')).toBe('native-import-startup-age-hint native-import-startup-age-error')
      expect(filterHost.querySelector('#native-import-startup-age-error')?.textContent).toContain('from 0 to 3650')
      type('0')
      expect(field.getAttribute('aria-describedby')).toBe('native-import-startup-age-hint')
      expect(filterHost.querySelector('#native-import-startup-age-hint')?.textContent).toBe('Older sessions stay in Discover histories. 0 = no limit.')
      expect(saveButton().disabled).toBe(false)
      await act(async () => saveButton().click())
      expect(save).toHaveBeenLastCalledWith({ titlePrefixes: [], directories: [], startupMaxAgeDays: 0 })
    } finally { act(() => filterRoot.unmount()); filterHost.remove() }
  })

  it('reports unsaved rules and follows saved rules changed elsewhere while not editing', () => {
    useAppStore.setState({ nativeImportExcludedTitlePrefixes: [], nativeImportExcludedDirectories: [] })
    const dirty = vi.fn()
    const filterHost = document.createElement('div'); document.body.append(filterHost)
    const filterRoot = createRoot(filterHost)
    try {
      act(() => filterRoot.render(<NativeImportFilters onDirtyChange={dirty} />))
      act(() => useAppStore.setState({ nativeImportExcludedDirectories: ['/srv/pushed'] }))
      const field = filterHost.querySelector<HTMLTextAreaElement>('#native-import-directories')!
      expect(field.value).toBe('/srv/pushed')
      act(() => {
        Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(field, '/srv/draft')
        field.dispatchEvent(new Event('input', { bubbles: true }))
      })
      expect(dirty).toHaveBeenLastCalledWith(true)
      act(() => useAppStore.setState({ nativeImportExcludedDirectories: ['/srv/other'] }))
      expect(field.value).toBe('/srv/draft')
    } finally { act(() => filterRoot.unmount()); filterHost.remove() }
    expect(dirty).toHaveBeenLastCalledWith(false)
  })
})

describe('Chat Data native discovery', () => {
  it('clears a cached-results error after a later successful read', async () => {
    useAppStore.setState({ startupNativeImports: page('ready') })
    mount()
    vi.spyOn(console, 'error').mockImplementation(() => {})
    await act(async () => requests[0].failure(409, 'Synthetic cache read failed'))
    expect(failures.at(-1)).toMatchObject({ message: 'Native history discovery results could not be read.', detail: 'Synthetic cache read failed' })
    act(() => useAppStore.setState({ startupNativeImports: { ...page('ready'), totalCount: 1 } }))
    await answer(1, { ...page('ready'), totalCount: 1 })
    expect(failures).toHaveLength(1)
  })

  beforeEach(() => {
    requests = []
    useAppStore.setState({ providers: SUPPORTED_CLI_PROVIDER_IDS.map(fallbackProviderForId), startupNativeImports: EMPTY_STARTUP_NATIVE_IMPORT_STATUS, startupNativeImportReviewSequence: 0, startupNativeImportFolderId: null })
    window.cefQuery = ({ request, onSuccess, onFailure }) => { requests.push({ ...JSON.parse(request), success: onSuccess, failure: onFailure }) }
    host = document.createElement('div'); document.body.append(host); root = createRoot(host)
  })
  afterEach(() => {
    act(() => root.unmount()); host.remove(); window.cefQuery = undefined; vi.restoreAllMocks()
  })
  const scope = () => host.querySelector('.uam-native-scope')?.getAttribute('aria-label')
  it('groups per-conversation issues by provider and reason', async () => {
    useAppStore.setState({ startupNativeImports: page('ready') })
    mount()
    await answer(0, { ...page('ready'), errors: ['Antigravity conversation a1: nested context.', 'Antigravity conversation b2: nested context.', 'Antigravity conversation b2: nested context.', 'OpenCode history discovery failed: invalid session list.', 'One provider store could not be read.'] })
    const rows = Array.from(host.querySelectorAll('.uam-native-issues > li')).map((row) => row.firstElementChild?.textContent + '|' + row.children[1]?.textContent)
    expect(rows).toEqual(['Antigravity|2 conversations skipped. Nested context.', 'OpenCode|History discovery failed: invalid session list.', 'Discovery|One provider store could not be read.'])
    expect(host.textContent).toContain('5 discovery issues')
    // The issues disclosure keeps its warning tone, unlike the neutral How discovery works.
    expect(Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent?.includes('5 discovery issues'))!.querySelector<SVGElement>('svg[style]')!.style.color).toBe('var(--warning)')
  })
  const mount = () => act(() => root.render(<NativeHistorySettings />))

  it('starts discovery only on click and prevents duplicate requests without importing', async () => {
    mount()
    expect(requests).toHaveLength(0)
    expect(scope()).toContain('Antigravity native history stores')
    expect(host.textContent).toContain('Chats that changed, are nested or are unrecognised are skipped')
    expect(host.textContent).toContain('unregistered local workspaces')
    act(() => { button('Discover histories').click(); button('Discover histories').click() })
    expect(requests).toHaveLength(1)
    expect(requests[0]).toMatchObject({ action: 'discoverSystemNativeImports', payload: {} })
    await answer(0, page())
    expect(button('Discover histories').disabled).toBe(true)
    expect(host.textContent).toContain('Scanning native histories')
    expect(requests.map((request) => request.action)).toEqual(['discoverSystemNativeImports', 'getStartupNativeImports'])
    await answer(1, page())
  })

  it('retains a newer pushed result when the discovery acknowledgement arrives late', async () => {
    mount()
    act(() => button('Discover histories').click())
    act(() => useAppStore.setState({ startupNativeImports: page('complete') }))
    await answer(1, { ...page('complete'), errors: ['One provider store could not be read.'] })
    await answer(0, page())
    expect(useAppStore.getState().startupNativeImports.phase).toBe('complete')
    expect(button('Discover histories').disabled).toBe(false)
    expect(host.textContent).toContain('0 histories awaiting review; 0 imported in this scan.')
    expect(host.textContent).toContain('One provider store could not be read.')
  })

  it('shows a rejected scan and leaves the existing staged generation available', async () => {
    useAppStore.setState({ startupNativeImports: { ...page('ready', 'startup'), totalCount: 1 } })
    mount()
    await answer(0, { ...page('ready', 'startup'), totalCount: 1 })
    act(() => button('Discover histories').click())
    vi.spyOn(console, 'error').mockImplementation(() => {})
    await act(async () => requests[1].failure(409, 'Wait for the current history operation.'))
    expect(failures.at(-1)).toMatchObject({ message: 'Native history discovery could not start.', detail: 'Wait for the current history operation.' })
    expect(useAppStore.getState().startupNativeImports.generation).toBe('startup')
    act(() => button('Review histories').click())
    expect(useAppStore.getState().startupNativeImportReviewSequence).toBe(1)
    expect(useAppStore.getState().startupNativeImportFolderId).toBeNull()
    expect(requests.map((request) => request.action)).toEqual(['getStartupNativeImports', 'discoverSystemNativeImports'])
  })

  it('refreshes bounded cached results after import without starting another discovery', async () => {
    useAppStore.setState({ startupNativeImports: page('importing') })
    mount(); await answer(0, page('importing'))
    expect(button('Discover histories').disabled).toBe(true)
    act(() => useAppStore.setState({ startupNativeImports: { ...page('complete'), importedCount: 2 } }))
    await answer(1, { ...page('complete'), importedCount: 2, errors: ['A remaining history failed.'] })
    expect(host.textContent).toContain('0 histories awaiting review; 2 imported in this scan.')
    expect(host.textContent).toContain('A remaining history failed.')
    expect(requests.every((request) => request.action === 'getStartupNativeImports')).toBe(true)
    expect(requests[1].payload).toEqual({ offset: 0, limit: 1 })
  })

  it('ignores cache replies from an older generation', async () => {
    useAppStore.setState({ startupNativeImports: page('complete', 'old') })
    mount()
    act(() => useAppStore.setState({ startupNativeImports: page('complete', 'new') }))
    await answer(1, page('complete', 'new'))
    await answer(0, { ...page('complete', 'old'), errors: ['Stale discovery error'] })
    expect(host.textContent).not.toContain('Stale discovery error')
  })

  it('reports the native provider catalogue without claiming excluded readers', () => {
    useAppStore.setState({ providers: [fallbackProviderForId('codex-cli')] })
    mount()
    expect(scope()).toBe('Reads Codex native history stores for this user.')
    expect(host.textContent).not.toContain('Antigravity')
    expect(host.textContent).not.toContain('OpenCode')
    act(() => useAppStore.setState({ providers: SUPPORTED_CLI_PROVIDER_IDS.map(fallbackProviderForId) }))
    expect(scope()).toContain('Antigravity native history stores for this user.')
    expect(host.textContent).toContain('OpenCode requires its configured CLI.')
    expect(requests).toHaveLength(0)
  })
})
