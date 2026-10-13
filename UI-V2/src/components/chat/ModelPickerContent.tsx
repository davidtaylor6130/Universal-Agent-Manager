import { Fragment, useEffect, useLayoutEffect, useMemo, useRef, useState, type KeyboardEvent } from 'react'
import { Check, Search, Star } from 'lucide-react'
import type { Provider } from '../../types/provider'
import type { ProviderModelCatalog } from '../../store/cpp/types'
import { useAppStore } from '../../store/useAppStore'
import { useModelPickerStore } from '../../store/modelPickerStore'
import { COPILOT_CLI_PROVIDER_ID, providerShortName } from '../../utils/providerMetadata'
import { ProviderLogo } from '../shared/ProviderLogo'
import '../layout/panels.css'
import { buildModelOptions, searchModelOptions, type ModelOption } from './modelOptions'

const ROW_HEIGHT = 52
const LIST_HEIGHT = 360
type PickerModel = ModelOption & { providerId: string }

/** Catalogues are owned by a provider, execution host and exact workspace. */
export function matchingModelCatalog(catalogs: ProviderModelCatalog[], providerId: string, hostId: string, workspace: string): ProviderModelCatalog | undefined {
  const key = (path: string) => {
    const normalized = path.replace(/\\/g, '/').replace(/\/+$/, '')
    return /^[a-z]:\//i.test(normalized) ? normalized.toLowerCase() : normalized
  }
  return catalogs.find((catalog) => catalog.providerId === providerId && (catalog.executionHostId || 'local') === hostId && key(catalog.workspaceDirectory) === key(workspace))
}

/** T3-style provider rail and model list; browsing never changes the running provider. */
export function ModelPickerContent({ providers, providerId, currentModelId, activeOptions, activeLoading, activeError, modelDisabled, canChangeProvider, workspaceDirectory, executionHostId, listHeight = LIST_HEIGHT, onSelect, onClose }: {
  providers: Provider[]
  providerId: string
  currentModelId: string
  activeOptions: ModelOption[]
  activeLoading?: boolean
  activeError?: string
  modelDisabled: boolean
  canChangeProvider: boolean
  workspaceDirectory: string
  executionHostId: string
  listHeight?: number
  onSelect:(providerId: string, modelId: string) => void
  onClose: () => void
}) {
  const catalogs = useAppStore((state) => state.providerModelCatalogs)
  const defaults = useAppStore((state) => state.providerChatDefaults)
  const discover = useAppStore((state) => state.discoverProviderModels)
  const favorites = useModelPickerStore((state) => state.favorites)
  const toggleFavorite = useModelPickerStore((state) => state.toggleFavorite)
  const [selectedProvider, setSelectedProvider] = useState(providerId)
  const [query, setQuery] = useState('')
  const [highlight, setHighlight] = useState(0)
  const [scrollTop, setScrollTop] = useState(0)
  const searchRef = useRef<HTMLInputElement>(null)
  const listRef = useRef<HTMLDivElement>(null)
  const rowRefs = useRef(new Map<number, HTMLButtonElement>())
  const railRefs = useRef(new Map<string, HTMLButtonElement>())
  const [glideY, setGlideY] = useState(0)
  const pendingFocus = useRef(false)
  const requestedCatalogs = useRef(new Set<string>())
  const optionsByProvider = useMemo(() => new Map(providers.map((provider) => {
    const catalog = matchingModelCatalog(catalogs, provider.id, executionHostId, workspaceDirectory)
    const options = provider.id === providerId ? activeOptions
      : buildModelOptions(catalog, '', provider, provider.id, true, defaults[provider.id])
    return [provider.id, options.map((option) => ({ ...option, providerId: provider.id }))]
  })), [providers, catalogs, executionHostId, workspaceDirectory, providerId, activeOptions, defaults])
  const models = useMemo(() => {
    const candidates = selectedProvider === 'favorites'
      ? favorites.flatMap((favorite) => {
          if (!optionsByProvider.has(favorite.providerId)) return []
          const option = optionsByProvider.get(favorite.providerId)?.find((option) => option.id === favorite.modelId)
          return option ? [option] : [{ id: favorite.modelId, providerId: favorite.providerId, label: favorite.modelId || 'Default', shortLabel: favorite.modelId, detail: 'Not available in this workspace', disabled: true }]
        })
      : optionsByProvider.get(selectedProvider) ?? []
    const matching = new Set(searchModelOptions(candidates, query).map((option) => option))
    return candidates.filter((option) => matching.has(option))
  }, [selectedProvider, favorites, optionsByProvider, query])
  const catalog = matchingModelCatalog(catalogs, selectedProvider, executionHostId, workspaceDirectory)
  const loading = selectedProvider === providerId ? activeLoading : catalog?.modelsLoading
  const error = selectedProvider === providerId ? activeError : catalog?.modelRefreshError
  const disabled = (option: PickerModel) => Boolean(option.disabled || modelDisabled || (option.providerId !== providerId && !canChangeProvider))
  // Favourites group under provider headers, so they render unwindowed (small lists); flat provider lists window.
  const grouped = selectedProvider === 'favorites'
  const virtual = !grouped && models.length > 60
  const first = virtual ? Math.max(0, Math.min(models.length - 1, Math.floor(scrollTop / ROW_HEIGHT) - 4)) : 0
  const last = virtual ? Math.min(models.length, first + Math.ceil(listHeight / ROW_HEIGHT) + 8) : models.length

  useEffect(() => { searchRef.current?.focus() }, [])
  useLayoutEffect(() => {
    const button = railRefs.current.get(selectedProvider)
    if (button) setGlideY(button.offsetTop + (button.offsetHeight - 20) / 2)
  }, [selectedProvider, providers])
  useLayoutEffect(() => {
    if (!pendingFocus.current) return
    pendingFocus.current = false
    rowRefs.current.get(highlight)?.focus()
  }, [highlight, scrollTop, models])

  const selectProvider = (id: string) => {
    setSelectedProvider(id)
    setQuery('')
    setHighlight(0)
    setScrollTop(0)
    if (listRef.current) listRef.current.scrollTop = 0
    if (id !== 'favorites' && id !== providerId && !requestedCatalogs.current.has(id)) {
      requestedCatalogs.current.add(id)
      void discover('', id, workspaceDirectory, executionHostId)
    }
  }
  const choose = (option: PickerModel | undefined) => { if (option && !disabled(option)) onSelect(option.providerId, option.id) }
  const focusModel = (index: number) => {
    pendingFocus.current = true
    if (virtual) {
      const list = listRef.current
      let top = list?.scrollTop ?? 0
      if (index * ROW_HEIGHT < top) top = index * ROW_HEIGHT
      else if ((index + 1) * ROW_HEIGHT > top + listHeight) top = (index + 1) * ROW_HEIGHT - listHeight
      if (list) list.scrollTop = top
      setScrollTop(top)
    }
    setHighlight(index)
    rowRefs.current.get(index)?.focus()
  }
  const onKeyDown = (event: KeyboardEvent<HTMLDivElement>) => {
    if (event.key === 'Escape') { event.preventDefault(); onClose(); return }
    if (event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return
    const inSearch = event.target === searchRef.current
    const inModels = Array.from(rowRefs.current.values()).includes(event.target as HTMLButtonElement)
    if (!inSearch && !inModels) return
    if (event.key === 'ArrowLeft' && !query) {
      event.preventDefault()
      railRefs.current.get(selectedProvider)?.focus()
      return
    }
    const selectable = models.flatMap((option, index) => disabled(option) ? [] : [index])
    if (event.key === 'Enter') {
      if (event.nativeEvent.isComposing) return
      event.preventDefault()
      choose(models[inSearch ? selectable[0] : highlight])
      return
    }
    if (selectable.length === 0 || !['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) return
    if (inSearch && (event.key === 'Home' || event.key === 'End')) return
    event.preventDefault()
    const position = selectable.indexOf(highlight)
    focusModel(event.key === 'Home' ? selectable[0] : event.key === 'End' ? selectable[selectable.length - 1]
      : inSearch ? event.key === 'ArrowDown' ? selectable[0] : selectable[selectable.length - 1]
      : selectable[(position + (event.key === 'ArrowDown' ? 1 : -1) + selectable.length) % selectable.length])
  }

  const providerLabel = (id: string) => id === 'favorites' ? 'Favourites' : providerShortName(providers.find((provider) => provider.id === id), id)
  return <div className="uam-model-picker flex min-h-[300px]" onKeyDown={onKeyDown}>
    <div role="toolbar" aria-label="Providers" aria-orientation="vertical" data-model-picker-sidebar="true"
      className="uam-np-rail w-11 shrink-0 overflow-x-hidden overflow-y-auto border-r p-1" style={{ borderColor: 'var(--border)', background: 'var(--surface-high)', '--np-glide-y': `${glideY}px` } as React.CSSProperties}
      onKeyDown={(event) => {
        if (event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return
        if (event.key === 'ArrowRight') { event.preventDefault(); searchRef.current?.focus(); return }
        if (!['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) return
        event.preventDefault()
        const ids = ['favorites', ...providers.filter((provider) => provider.id === providerId || canChangeProvider).map((provider) => provider.id)]
        const position = ids.indexOf(selectedProvider)
        const id = event.key === 'Home' ? ids[0] : event.key === 'End' ? ids[ids.length - 1] : ids[(position + (event.key === 'ArrowDown' ? 1 : -1) + ids.length) % ids.length]
        selectProvider(id)
        railRefs.current.get(id)?.focus()
      }}>
      <span aria-hidden className="uam-np-rail__glide" />
      {[{ id: 'favorites', label: 'Favorites' }, ...providers.map((provider) => ({ id: provider.id, label: providerShortName(provider, provider.id) }))].map((entry) => <button
        key={entry.id} ref={(element) => { if (element) railRefs.current.set(entry.id, element); else railRefs.current.delete(entry.id) }}
        type="button" aria-label={entry.label} title={entry.label} aria-pressed={entry.id === selectedProvider}
        disabled={entry.id !== 'favorites' && entry.id !== providerId && !canChangeProvider}
        tabIndex={entry.id === selectedProvider ? 0 : -1}
        onClick={() => selectProvider(entry.id)}
        className="uam-np-rail__button"
        style={{ opacity: entry.id !== 'favorites' && entry.id !== providerId && !canChangeProvider ? 0.4 : 1 }}>
        {entry.id === 'favorites' ? <Star size={18} fill="currentColor" aria-hidden /> : <ProviderLogo providerId={entry.id} size={20} />}
      </button>)}
    </div>
    <div className="min-w-0 flex-1 p-1">
      <div className="uam-np-search">
        <Search size={13} aria-hidden />
        <input ref={searchRef} type="search" aria-label="Search models" placeholder="Search models…" value={query}
          onChange={(event) => { setQuery(event.target.value); setHighlight(0); setScrollTop(0); if (listRef.current) listRef.current.scrollTop = 0 }} />
      </div>
      {!grouped && <div className="uam-np-group">{providerLabel(selectedProvider)}</div>}
      {loading && <div role="status" className="px-2 py-1 text-xs" style={{ color: 'var(--accent)' }}>Discovering models…</div>}
      {error && <div role="status" className="px-2 py-1 text-xs" style={{ color: 'var(--yellow)' }}>{error}</div>}
      <div ref={listRef} data-testid="model-options" style={{ maxHeight: listHeight, overflowY: 'auto' }} onScroll={(event) => setScrollTop(event.currentTarget.scrollTop)}>
        {models.length === 0 && (
          <div role="status" className="uam-np-empty">
            <span className="uam-np-empty__icon">{grouped && !query ? <Star size={16} aria-hidden /> : <Search size={16} aria-hidden />}</span>
            <span className="uam-np-empty__title">{grouped && !query ? 'No favourites yet' : 'No matching models'}</span>
            <span className="uam-np-empty__text">{grouped && !query ? 'Star a model to add it here' : query ? `Nothing matches “${query}”. Try a shorter search.` : 'This provider has no models to show.'}</span>
          </div>
        )}
        {virtual && <div aria-hidden style={{ height: first * ROW_HEIGHT }} />}
        {models.slice(first, last).map((option, offset) => {
          const index = first + offset
          const selected = option.providerId === providerId && option.id === currentModelId
          const favorite = favorites.some((entry) => entry.providerId === option.providerId && entry.modelId === option.id)
          const sectionStart = grouped && (index === 0 || models[index - 1].providerId !== option.providerId)
          const highlighted = index === highlight
          return <Fragment key={JSON.stringify([option.providerId, option.id])}>
            {sectionStart && <div className="uam-np-group" role="presentation">{providerLabel(option.providerId)}</div>}
            <div className="uam-np-model" data-selected={selected} data-highlight={highlighted} style={{ height: ROW_HEIGHT }}>
              <button ref={(element) => { if (element) rowRefs.current.set(index, element); else rowRefs.current.delete(index) }} type="button" role="menuitemradio" aria-checked={selected}
                disabled={disabled(option)} tabIndex={highlighted ? 0 : -1} onFocus={() => setHighlight(index)} onMouseEnter={() => setHighlight(index)} onClick={() => choose(option)}
                className="uam-np-model__button" style={{ opacity: disabled(option) ? 0.5 : 1 }}>
                <span className="uam-np-model__name">
                  <span>{option.label}</span>
                  {option.id && option.id !== option.label && <code className="uam-np-model__id">{option.id}</code>}
                  {selected && <Check size={14} aria-hidden className="uam-np-model__check" />}
                </span>
                {!(option.providerId === COPILOT_CLI_PROVIDER_ID && option.detail.trim().toLowerCase() === option.label.trim().toLowerCase()) && <span className="uam-np-model__detail">{option.detail}</span>}
              </button>
              <button type="button" aria-label={`${favorite ? 'Unfavorite' : 'Favorite'} ${option.label}`} aria-pressed={favorite} title={favorite ? 'Remove favorite' : 'Add favorite'}
                onClick={() => toggleFavorite(option.providerId, option.id)} className="uam-np-model__fav">
                <Star size={14} fill={favorite ? 'currentColor' : 'none'} aria-hidden />
              </button>
            </div>
          </Fragment>
        })}
        {virtual && <div aria-hidden style={{ height: (models.length - last) * ROW_HEIGHT }} />}
      </div>
    </div>
  </div>
}
