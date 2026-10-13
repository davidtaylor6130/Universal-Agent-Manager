import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { ModelPickerContent, matchingModelCatalog } from './ModelPickerContent'
import { useAppStore } from '../../store/useAppStore'
import { useModelPickerStore, normalizeFavoriteModels } from '../../store/modelPickerStore'
import type { Provider } from '../../types/provider'
import type { ProviderModelCatalog } from '../../store/cpp/types'

globalThis.IS_REACT_ACT_ENVIRONMENT = true
const providers: Provider[] = ['codex-cli', 'claude-cli'].map((id) => ({ id, name: id, shortName: id === 'codex-cli' ? 'Codex' : 'Claude', color: '', description: '' }))
const models = Array.from({ length: 120 }, (_, index) => ({ id: `model-${index}`, label: `Model ${index}`, shortLabel: `Model ${index}`, detail: `Description ${index}`, disabled: index === 1 }))
const catalog = (host: string, workspace: string, id: string): ProviderModelCatalog => ({ providerId: 'claude-cli', executionHostId: host, workspaceDirectory: workspace, availableModels: [{ id, name: id, description: '', supportedReasoningEfforts: [] }], currentModelId: '', modelsLoading: false, modelRefreshError: '' })

beforeEach(() => {
  useModelPickerStore.setState({ favorites: [] })
  useAppStore.setState({ providerModelCatalogs: [catalog('local', '/project', 'local-only'), catalog('ssh-lab', '/project', 'remote-only')], providerChatDefaults: {}, discoverProviderModels: vi.fn().mockResolvedValue(true) })
})

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
    act(() => root.render(<ModelPickerContent providers={providers} providerId="codex-cli" currentModelId="model-0" activeOptions={models} modelDisabled={false} canChangeProvider={false} workspaceDirectory="/project" executionHostId="local" onSelect={onSelect} onClose={() => {}} />))
    const search = host.querySelector<HTMLInputElement>('input')!
    expect(document.activeElement).toBe(search)
    expect(host.querySelectorAll('[role="menuitemradio"]').length).toBeLessThan(25)
    expect(host.querySelector<HTMLButtonElement>('[aria-label="Claude"]')!.disabled).toBe(true)
    act(() => search.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
    act(() => document.activeElement!.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
    expect(document.activeElement?.textContent).toContain('Model 2')
    act(() => document.activeElement!.dispatchEvent(new KeyboardEvent('keydown', { key: 'End', bubbles: true })))
    expect(document.activeElement?.textContent).toContain('Model 119')
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
