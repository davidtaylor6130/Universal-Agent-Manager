import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ModelPickerContent, matchingModelCatalog } from './ModelPickerContent'
import { buildModelOptions } from './modelOptions'
import { useAppStore } from '../../store/useAppStore'
import { useModelPickerStore, normalizeFavoriteModels } from '../../store/modelPickerStore'
import type { Provider } from '../../types/provider'
import type { ProviderModelCatalog } from '../../store/cpp/types'

globalThis.IS_REACT_ACT_ENVIRONMENT = true
const providers: Provider[] = ['codex-cli', 'claude-cli'].map((id) => ({ id, name: id, shortName: id === 'codex-cli' ? 'Codex' : 'Claude', color: '', description: '' }))
const models = Array.from({ length: 120 }, (_, index) => ({ id: `model-${index}`, label: `Model ${index}`, shortLabel: `Model ${index}`, detail: `Description ${index}`, disabled: index === 1 }))
const catalog = (host: string, workspace: string, id: string): ProviderModelCatalog => ({ providerId: 'claude-cli', executionHostId: host, workspaceDirectory: workspace, availableModels: [{ id, name: id, description: '', supportedReasoningEfforts: [] }], currentModelId: '', modelsLoading: false, modelRefreshError: '' })
const capacityValues = (row: Element) => Array.from(row.querySelectorAll('.uam-np-model__capacity-value'), value => value.textContent)

beforeEach(() => {
  vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
  useModelPickerStore.setState({ favorites: [] })
  useAppStore.setState({ providerModelCatalogs: [catalog('local', '/project', 'local-only'), catalog('ssh-lab', '/project', 'remote-only')], providerChatDefaults: {}, discoverProviderModels: vi.fn().mockResolvedValue(true) })
})

afterEach(() => vi.unstubAllGlobals())

describe('T3-style model picker', () => {
  it('browses providers without selecting, isolates catalogues and preserves favorite identity', () => {
    const onSelect = vi.fn()
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    act(() => root.render(<ModelPickerContent providers={providers} providerId="codex-cli" currentModelId="model-0" activeOptions={models.slice(0, 3)} modelDisabled={false} canChangeProvider workspaceDirectory="/project" executionHostId="ssh-lab" onSelect={onSelect} onClose={() => {}} />))
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Claude"]')!.click())
    expect(onSelect).not.toHaveBeenCalled()
    expect(host.textContent).toContain('remote-only')
    expect(host.textContent).not.toContain('local-only')
    expect(useAppStore.getState().discoverProviderModels).not.toHaveBeenCalled()
    act(() => useAppStore.setState({ providerModelCatalogs: [{ ...catalog('ssh-lab', '/project', 'remote-only'), modelsLoading: true, modelRefreshError: 'Fixture refresh pending' }] }))
    expect(host.textContent).toContain('remote-only')
    expect(host.textContent).toContain('Discovering models')
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Favorite remote-only"]')!.click())
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Favorites"]')!.click())
    const option = host.querySelector<HTMLButtonElement>('[role="menuitemradio"]')!
    expect(option.textContent).toContain('remote-only')
    act(() => option.click())
    expect(onSelect).toHaveBeenCalledWith('claude-cli', 'remote-only')
    act(() => root.unmount()); host.remove()
  })

  it('bounds large lists and keyboard navigation skips disabled models and reaches unmounted rows', () => {
    const onSelect = vi.fn()
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    const capacityModels = models.map((model, index) => ({ ...model, contextWindowTokens: (index + 1) * 1000, maxOutputTokens: 8192 }))
    act(() => root.render(<ModelPickerContent providers={providers} providerId="codex-cli" currentModelId="model-0" activeOptions={capacityModels} modelDisabled={false} canChangeProvider={false} workspaceDirectory="/project" executionHostId="local" onSelect={onSelect} onClose={() => {}} />))
    const search = host.querySelector<HTMLInputElement>('input')!
    expect(document.activeElement).toBe(search)
    expect(host.querySelectorAll('[role="menuitemradio"]').length).toBeLessThan(25)
    expect(host.querySelector<HTMLButtonElement>('[aria-label="Claude"]')!.disabled).toBe(true)
    act(() => search.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
    act(() => document.activeElement!.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
    expect(document.activeElement?.textContent).toContain('Model 2')
    act(() => document.activeElement!.dispatchEvent(new KeyboardEvent('keydown', { key: 'End', bubbles: true })))
    expect(document.activeElement?.textContent).toContain('Model 119')
    expect(capacityValues(document.activeElement!)).toEqual(['120K', '8.2K'])
    expect((document.activeElement?.parentElement as HTMLElement).style.height).toBe('52px')
    act(() => document.activeElement!.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
    expect(onSelect).toHaveBeenCalledWith('codex-cli', 'model-119')
    expect(host.querySelectorAll('[role="menuitemradio"]').length).toBeLessThan(25)
    act(() => root.unmount()); host.remove()
  })

  it('does not merge case-sensitive workspaces or accept malformed favorites', () => {
    expect(matchingModelCatalog([catalog('local', '/Project', 'wrong')], 'claude-cli', 'local', '/project')).toBeUndefined()
    expect(matchingModelCatalog([catalog('local', 'C:\\Project\\', 'right')], 'claude-cli', 'local', 'c:/project')?.availableModels[0].id).toBe('right')
    expect(normalizeFavoriteModels([null, { providerId: '', modelId: 'bad' }, { providerId: 'p', modelId: 2 }, { providerId: 'p', modelId: 'm' }, { providerId: 'p', modelId: 'm' }])).toEqual([{ providerId: 'p', modelId: 'm' }])
    expect(normalizeFavoriteModels(Array.from({ length: 300 }, (_, index) => ({ providerId: 'p', modelId: String(index) })))).toHaveLength(200)
  })
})

describe('model capacity badges', () => {
  it.each(['option', 'picker'])('retains disabled descriptions and exact limits in narrow rows (%s disabled)', (disabledBy) => {
    const detail = 'This reported model is unavailable for selection'
    const onSelect = vi.fn()
    const host = document.createElement('div'); host.style.width = '280px'; document.body.append(host)
    const root = createRoot(host)
    const render = (blocked: boolean) => act(() => root.render(<ModelPickerContent providers={providers} providerId="codex-cli" currentModelId="reported"
      activeOptions={[{ id: 'reported', label: 'Reported model', shortLabel: 'Reported', detail, disabled: disabledBy === 'option' && blocked, contextWindowTokens: 128000, maxOutputTokens: 4096 }]}
      modelDisabled={disabledBy === 'picker' && blocked} canChangeProvider workspaceDirectory="/project" executionHostId="local" onSelect={onSelect} onClose={() => {}} />))
    render(true)
    const row = host.querySelector<HTMLButtonElement>('[role="menuitemradio"]')!
    expect(row.disabled).toBe(true)
    expect(row.getAttribute('aria-label')).toContain(detail)
    expect(row.getAttribute('aria-label')).toContain('Context window: 128,000 tokens. Maximum output: 4,096 tokens.')
    expect(row.title).toBe(row.getAttribute('aria-label'))
    expect(row.hasAttribute('data-state')).toBe(false)
    expect(row.hasAttribute('aria-describedby')).toBe(false)
    expect(capacityValues(row)).toEqual(['128K', '4.1K'])
    expect(row.parentElement?.style.height).toBe('52px')
    act(() => row.click())
    expect(onSelect).not.toHaveBeenCalled()
    render(false)
    const enabledRow = host.querySelector<HTMLButtonElement>('[role="menuitemradio"]')!
    expect(enabledRow.disabled).toBe(false)
    expect(enabledRow.hasAttribute('title')).toBe(false)
    expect(enabledRow.hasAttribute('aria-label')).toBe(false)
    act(() => enabledRow.click())
    expect(onSelect).toHaveBeenCalledWith('codex-cli', 'reported')
    act(() => root.unmount()); host.remove()
  })

  it('keeps exact limits in the row name without repeating them in its focused tooltip description', async () => {
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    act(() => root.render(<ModelPickerContent providers={providers} providerId="codex-cli" currentModelId="reported"
      activeOptions={[{ id: 'reported', label: 'Reported', shortLabel: 'Reported', detail: 'Full provider description', contextWindowTokens: 128000, maxOutputTokens: 4096 }]}
      modelDisabled={false} canChangeProvider workspaceDirectory="/project" executionHostId="local" onSelect={() => {}} onClose={() => {}} />))
    const row = host.querySelector<HTMLButtonElement>('[role="menuitemradio"]')!
    await act(async () => row.focus())
    const tooltip = document.getElementById(row.getAttribute('aria-describedby')!)!
    expect(tooltip).not.toBeNull()
    expect(row.querySelector('.sr-only')?.textContent).toContain('Context window: 128,000 tokens. Maximum output: 4,096 tokens.')
    expect(tooltip.textContent).toContain('Context window: 128,000 tokens.')
    const accessibleDescription = tooltip.cloneNode(true) as HTMLElement
    accessibleDescription.querySelectorAll('[aria-hidden="true"]').forEach(element => element.remove())
    expect(accessibleDescription.textContent).toContain('Full provider description')
    expect(accessibleDescription.textContent).not.toContain('Context window:')
    expect(accessibleDescription.textContent).not.toContain('Maximum output:')
    act(() => root.unmount()); host.remove()
  })

  it('labels known limits and explicit unknowns without guessing parameter size from a model name', () => {
    const source = { availableModels: [
      { id: 'reported', name: 'Reported', description: 'Reported limits', contextWindowTokens: 200000, maxOutputTokens: 0 },
      { id: 'model-70b-200k', name: 'Model 70B 200K', description: '', contextWindowTokens: null },
      { id: 'invalid', name: 'Invalid limits', description: '', contextWindowTokens: -1, maxOutputTokens: 1.5 },
    ] }
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    act(() => root.render(<ModelPickerContent providers={providers} providerId="codex-cli" currentModelId="reported" activeOptions={buildModelOptions(source, '', undefined, 'codex-cli', true)} modelDisabled={false} canChangeProvider workspaceDirectory="/project" executionHostId="local" onSelect={() => {}} onClose={() => {}} />))
    const rows = host.querySelectorAll<HTMLButtonElement>('[role="menuitemradio"]')
    expect(Array.from(rows, capacityValues)).toEqual([
      ['unknown', 'unknown'],
      ['200K', '0'],
      ['unknown', 'unknown'],
      ['unknown', 'unknown'],
    ])
    expect(rows[1].querySelector('.sr-only')?.textContent).toBe('Context window: 200,000 tokens. Maximum output: 0 tokens. Physical model size: not reported.')
    expect(rows[2].querySelector('.sr-only')?.textContent).toBe('Context window: unknown (not reported). Maximum output: unknown (not reported). Physical model size: not reported.')
    expect(rows[1].querySelector('.uam-np-model__capacities')?.getAttribute('aria-hidden')).toBe('true')
    act(() => root.unmount()); host.remove()
  })

  it('keeps provider and host/workspace limits separate through favourites and capacity-only refreshes', () => {
    const scoped = (host: string, workspace: string, contextWindowTokens: number): ProviderModelCatalog => ({
      ...catalog(host, workspace, 'shared-model'),
      availableModels: [{ id: 'shared-model', name: 'Shared model', description: '', contextWindowTokens, maxOutputTokens: 64000 }],
    })
    useAppStore.setState({ providerModelCatalogs: [scoped('local', '/project', 300000), scoped('ssh-lab', '/other-project', 400000), scoped('ssh-lab', '/project', 1000000)] })
    useModelPickerStore.setState({ favorites: [
      { providerId: 'codex-cli', modelId: 'shared-model' },
      { providerId: 'claude-cli', modelId: 'shared-model' },
      { providerId: 'claude-cli', modelId: 'missing-model' },
    ] })
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    const render = (contextWindowTokens: number | null, workspace = '/project') => act(() => root.render(<ModelPickerContent providers={providers} providerId="codex-cli" currentModelId="shared-model"
      activeOptions={buildModelOptions({ availableModels: [{ id: 'shared-model', name: 'Shared model', description: '', contextWindowTokens, maxOutputTokens: 4096 }] }, '', undefined, 'codex-cli')}
      modelDisabled={false} canChangeProvider workspaceDirectory={workspace} executionHostId="ssh-lab" onSelect={() => {}} onClose={() => {}} />))
    const badges = () => Array.from(host.querySelectorAll('[role="menuitemradio"]'), capacityValues)
    render(200000)
    expect(badges()).toEqual([['200K', '4.1K']])
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Claude"]')!.click())
    expect(badges()).toEqual([['unknown', 'unknown'], ['1M', '64K']])
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Favorites"]')!.click())
    expect(badges()).toEqual([['200K', '4.1K'], ['1M', '64K'], ['unknown', 'unknown']])
    expect(host.querySelectorAll<HTMLButtonElement>('[role="menuitemradio"]')[2].disabled).toBe(true)
    act(() => useAppStore.setState({ providerModelCatalogs: [scoped('ssh-lab', '/project', 128000)] }))
    render(null)
    expect(badges()).toEqual([['unknown', '4.1K'], ['128K', '64K'], ['unknown', 'unknown']])
    render(null, '/unavailable-project')
    expect(badges()).toEqual([['unknown', '4.1K'], ['unknown', 'unknown'], ['unknown', 'unknown']])
    expect(host.querySelectorAll<HTMLButtonElement>('[role="menuitemradio"]')[1].disabled).toBe(true)
    expect(useAppStore.getState().discoverProviderModels).not.toHaveBeenCalled()
    act(() => root.unmount()); host.remove()
  })
})


describe('scoped model catalogue reuse', () => {
  it('fetches empty scopes once and gives changed host or workspace its own request', () => {
    useAppStore.setState({ providerModelCatalogs: [] })
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    const render = (executionHostId: string, workspaceDirectory: string) => act(() => root.render(<ModelPickerContent providers={providers} providerId="codex-cli" currentModelId="" activeOptions={[]} modelDisabled={false} canChangeProvider workspaceDirectory={workspaceDirectory} executionHostId={executionHostId} onSelect={() => {}} onClose={() => {}} />))
    const browse = () => {
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Codex"]')!.click())
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Claude"]')!.click())
    }
    render('ssh-lab', '/project'); browse(); browse()
    expect(useAppStore.getState().discoverProviderModels).toHaveBeenCalledTimes(1)
    render('ssh-other', '/project'); browse()
    render('ssh-other', '/other-project'); browse()
    expect(useAppStore.getState().discoverProviderModels).toHaveBeenCalledTimes(3)
    expect(useAppStore.getState().discoverProviderModels).toHaveBeenLastCalledWith('', 'claude-cli', '/other-project', 'ssh-other')
    act(() => root.unmount()); host.remove()
  })

  it('uses the same normalized catalogue identity without folding POSIX literal backslashes', () => {
    expect(matchingModelCatalog([catalog('ssh-lab', '/project\\name', 'literal')], 'claude-cli', 'ssh-lab', '/project/name')).toBeUndefined()
    expect(matchingModelCatalog([catalog('local', 'C:\\Project\\', 'windows')], 'claude', 'local', 'c:/project')?.availableModels[0].id).toBe('windows')
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    useAppStore.setState({ providerModelCatalogs: [] })
    const render = (workspaceDirectory: string) => act(() => root.render(<ModelPickerContent providers={providers} providerId="codex-cli" currentModelId="" activeOptions={[]} modelDisabled={false} canChangeProvider workspaceDirectory={workspaceDirectory} executionHostId="ssh-lab" onSelect={() => {}} onClose={() => {}} />))
    render('C:\\Project\\')
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Claude"]')!.click())
    render('c:/project')
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Codex"]')!.click())
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Claude"]')!.click())
    expect(useAppStore.getState().discoverProviderModels).toHaveBeenCalledTimes(1)
    act(() => root.unmount()); host.remove()
  })
})


it('reuses populated model configuration choices without automatic refresh', () => {
  useAppStore.setState({ providerModelCatalogs: [{ ...catalog('ssh-lab', '/project', 'unused'), availableModels: [], configOptions: [{ id: 'model', category: 'model', name: 'Model', description: '', currentValue: 'config-model', options: [{ value: 'config-model', name: 'Configuration model', description: '' }] }] }] })
  const host = document.createElement('div'); document.body.append(host)
  const root = createRoot(host)
  act(() => root.render(<ModelPickerContent providers={providers} providerId="codex-cli" currentModelId="" activeOptions={[]} modelDisabled={false} canChangeProvider workspaceDirectory="/project" executionHostId="ssh-lab" onSelect={() => {}} onClose={() => {}} />))
  act(() => host.querySelector<HTMLButtonElement>('[aria-label="Claude"]')!.click())
  expect(host.textContent).toContain('Configuration model')
  expect(useAppStore.getState().discoverProviderModels).not.toHaveBeenCalled()
  act(() => root.unmount()); host.remove()
})


it('uses Windows host metadata for UNC identity while preserving POSIX scopes', () => {
  expect(matchingModelCatalog([catalog('win-host', '//SERVER/Share/Project', 'unc')], 'claude-cli', 'win-host', '\\\\server\\share\\project\\', 'windows')?.availableModels[0].id).toBe('unc')
  expect(matchingModelCatalog([catalog('linux-host', '/project\\name', 'literal')], 'claude-cli', 'linux-host', '/project/name', 'linux')).toBeUndefined()
  expect(matchingModelCatalog([catalog('linux-host', '/Project', 'case')], 'claude-cli', 'linux-host', '/project', 'linux')).toBeUndefined()
  const host = document.createElement('div'); document.body.append(host)
  const root = createRoot(host)
  useAppStore.setState({ executionHosts: [{ id: 'win-host', label: 'Windows fixture', transport: 'ssh', sshAlias: 'never-connect', runnerStatus: 'ready', runnerVersion: '', platform: 'windows', architecture: '', lastSeenAt: '' }], providerModelCatalogs: [catalog('win-host', '//SERVER/Share/Project', 'unc')] })
  act(() => root.render(<ModelPickerContent providers={providers} providerId="codex-cli" currentModelId="" activeOptions={[]} modelDisabled={false} canChangeProvider workspaceDirectory={'\\\\server\\share\\project'} executionHostId="win-host" onSelect={() => {}} onClose={() => {}} />))
  act(() => host.querySelector<HTMLButtonElement>('[aria-label="Claude"]')!.click())
  expect(host.textContent).toContain('unc')
  expect(useAppStore.getState().discoverProviderModels).not.toHaveBeenCalled()
  act(() => root.unmount()); host.remove()
})
