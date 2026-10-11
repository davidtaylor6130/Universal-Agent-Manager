import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { USAGE_PROVIDERS, type UsageTotals } from '../../services/usageHistory'
import { useAppStore } from '../../store/useAppStore'
import { UsagePage, parseUsagePrices } from './UsagePage'
import { UsageLimits } from './UsageLimits'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const values = new Map<string, string>()
const total: UsageTotals = { inputTokens: 10, cachedInputTokens: 5, cacheWriteTokens: 0, outputTokens: 2, reasoningTokens: 1, totalTokens: 17,
  reportedCostUsd: 0, estimatedCostUsd: 0, estimatedCacheSavingsUsd: 0, reportedCostTokens: 0, estimatedCostTokens: 0, unpricedTokens: 17, records: 1, sessions: 1 }
const blank = { ...total, inputTokens: 0, cachedInputTokens: 0, outputTokens: 0, reasoningTokens: 0, totalTokens: 0, unpricedTokens: 0, records: 0, sessions: 0 }
afterEach(() => { delete window.cefQuery; vi.restoreAllMocks(); values.clear() })
function storage() {
  vi.spyOn(window, 'localStorage', 'get').mockReturnValue({ getItem: key => values.get(key) ?? null, setItem: (key, value) => { values.set(key, value) }, removeItem: key => { values.delete(key) }, clear: () => values.clear(), length: 0, key: () => null })
}

describe('UsagePage', () => {
  it('prefills reference prices, exposes their source, and preserves explicit manual blanks and zero', async () => {
    storage()
    const reference = { kind: 'reference', referenceModel: 'qwen3.8-27b', source: 'https://example.com/qwen', verifiedDate: '2026-10-10', cacheFallback: true, speedFallback: true, inputPerMillion: 0.5, cachedInputPerMillion: 0.1, cacheWritePerMillion: 0.625, outputPerMillion: 3 }
    window.cefQuery = request => {
      const { payload } = JSON.parse(request.request)
      request.onSuccess(JSON.stringify({ version: 2, executionHostId: payload.executionHostId, source: 'native-transcripts', startMs: payload.startMs, endMs: payload.endMs, totals: total, complete: true, listingLimited: false,
        filesChecked: 1, filesTotal: 1, bytesRead: 0, duplicates: 0, unidentifiedRecords: 0,
        providers: USAGE_PROVIDERS.map(providerId => ({ ...(providerId === 'codex-cli' ? total : blank), providerId, available: true, files: 1, errors: 0, invalidLines: 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 })),
        models: [{ ...total, providerId: 'codex-cli', modelId: 'test-model', speed: 'standard', priceReference: reference }], days: [], hours: [], providerDays: [], providerHours: [] }))
    }
    for (const manual of [false, true]) {
      if (manual) values.set('uam-usage-prices-v1', JSON.stringify([{ providerId: 'codex-cli', modelId: 'test-model', speed: 'standard', inputPerMillion: 0, cachedInputPerMillion: null, cacheWritePerMillion: null, outputPerMillion: 2 }]))
      const host = document.createElement('div'), root = createRoot(host)
      try {
        await act(async () => root.render(<UsagePage onClose={() => {}} />))
        act(() => host.querySelector<HTMLButtonElement>('[aria-label="Inspect Codex test-model standard"]')!.click())
        act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Model prices')!.click())
        expect(Array.from(host.querySelectorAll<HTMLInputElement>('input')).map(input => input.value)).toEqual(manual ? ['0', '', '', '2'] : ['0.5', '0.1', '0.625', '3'])
        expect(host.querySelector('a')?.getAttribute('href')).toBe('https://example.com/qwen')
        expect(host.textContent).toContain('Unpublished cache rates use the input rate.')
        expect(host.textContent).toContain('This speed uses the standard rate')
      } finally { act(() => root.unmount()) }
    }
  })

  it('opens all machines, shows offline coverage, and returns there after removing the selected machine', async () => {
    storage()
    const previous = useAppStore.getState(), requests: string[] = []
    useAppStore.setState({ executionHosts: [{ id: 'local', label: 'This computer' }, { id: 'peer', label: 'Windows' }] as typeof previous.executionHosts })
    window.cefQuery = request => {
      const { payload } = JSON.parse(request.request); requests.push(payload.executionHostId)
      request.onSuccess(JSON.stringify({ version: 3, executionHostId: payload.executionHostId, source: 'native-transcripts', asOfMs: payload.endMs, windowDays: payload.windowDays, collecting: false, collectionError: '',
        startMs: payload.startMs, endMs: payload.endMs, totals: total, complete: false, listingLimited: false, filesChecked: 1, filesTotal: 1, bytesRead: 0, duplicates: 0, unidentifiedRecords: 0,
        coverage: [{ executionHostId: 'peer', label: 'Windows', status: 'cached', asOfMs: 1000, error: 'Machine is offline.' }],
        providers: USAGE_PROVIDERS.map(providerId => ({ ...(providerId === 'codex-cli' ? total : blank), providerId, available: true, files: 1, errors: 0, invalidLines: 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 })),
        models: [], days: [], hours: [], providerDays: [], providerHours: [] }))
    }
    const host = document.createElement('div'), root = createRoot(host); document.body.appendChild(host)
    try {
      await act(async () => root.render(<UsagePage onClose={() => {}} />))
      expect(requests[0]).toBe('*')
      expect(host.querySelector('[aria-label="Usage machine coverage"]')?.textContent).toContain('Windows: Cached')
      expect(host.textContent).toContain('Machine is offline.'); expect(host.textContent).not.toContain('Loading usage history')
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Usage host"]')!.click())
      await act(async () => Array.from(document.querySelectorAll<HTMLElement>('[role="option"]')).find(option => option.textContent?.includes('Windows'))!.click())
      expect(requests.at(-1)).toBe('peer')
      await act(async () => useAppStore.setState({ executionHosts: [{ id: 'local', label: 'This computer' }] as typeof previous.executionHosts }))
      expect(requests.at(-1)).toBe('*')
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState(previous, true) }
  })

  it('shows unknown costs, request diagnostics, token subsets and editable zero/blank rates', async () => {
    storage()
    const requests: Record<string, unknown>[] = []
    let modelCount = 1
    window.cefQuery = request => {
      const { payload } = JSON.parse(request.request)
      requests.push(payload)
      request.onSuccess(JSON.stringify({ version: 2, executionHostId: payload.executionHostId, source: 'native-transcripts', startMs: payload.startMs, endMs: payload.endMs, totals: total, complete: true, listingLimited: true,
        filesChecked: 1, filesTotal: 1, bytesRead: 0, duplicates: 1, unidentifiedRecords: 0,
        providers: USAGE_PROVIDERS.map(providerId => ({ ...(providerId === 'codex-cli' ? total : blank), providerId, available: providerId === 'codex-cli', files: providerId === 'codex-cli' ? 1 : 0, errors: 0, invalidLines: providerId === 'codex-cli' ? 1 : 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 })),
        models: Array.from({ length: modelCount }, (_, index) => ({ ...total, providerId: 'codex-cli', modelId: index === 0 ? 'test-model' : `model-${index}`, speed: 'standard' })), days: [], hours: [], providerDays: [], providerHours: [] }))
    }
    const host = document.createElement('div'), root = createRoot(host)
    document.body.appendChild(host)
    try {
      await act(async () => root.render(<UsagePage onClose={() => {}} />))
      expect(host.textContent).toContain('Unknown')
      expect(host.textContent).toContain('totals are incomplete')
      expect(host.textContent).toContain('1 invalid records')
      expect(host.textContent).toContain('Reasoning, included in output')
      const button = (text: string) => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === text)!
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Inspect Codex test-model standard"]')!.click())
      act(() => button('Model prices').click())
      const form = host.querySelector<HTMLFormElement>('[aria-label="Prices for test-model"]')!
      const input = form.querySelector<HTMLInputElement>('input')!
      act(() => {
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, '0')
        input.dispatchEvent(new Event('input', { bubbles: true }))
      })
      await act(async () => form.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })))
      expect(JSON.parse(values.get('uam-usage-prices-v1')!)[0]).toMatchObject({ inputPerMillion: 0, cachedInputPerMillion: null, outputPerMillion: null })
      expect(requests.at(-1)?.priceOverrides).toMatchObject([{ inputPerMillion: 0 }])
      modelCount = 201
      await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Refresh usage"]')!.click())
      expect(host.querySelectorAll('[aria-label^="Inspect"]')).toHaveLength(100)
      act(() => button('Show more models (101 remaining)').click())
      expect(host.querySelectorAll('[aria-label^="Inspect"]')).toHaveLength(200)
      const count = requests.length
      await act(async () => button('Limits').click())
      expect(requests).toHaveLength(count)
      expect(host.textContent).toContain('Not readable at this time')
    } finally { act(() => root.unmount()); host.remove() }
  })
  it('filters totals, chart bands and models together without rescanning history', async () => {
    storage()
    let requests = 0
    window.cefQuery = request => {
      requests++
      const { payload } = JSON.parse(request.request)
      const startMs = Math.floor(payload.startMs / 86400000) * 86400000
      const combined = Object.fromEntries(Object.entries(total).map(([key, value]) => [key, value * 2]))
      const providers = USAGE_PROVIDERS.map(providerId => ({ ...(['codex-cli', 'claude-cli'].includes(providerId) ? total : blank), providerId, available: ['codex-cli', 'claude-cli'].includes(providerId), files: 1, errors: 0, invalidLines: 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 }))
      request.onSuccess(JSON.stringify({ version: 2, executionHostId: payload.executionHostId, source: 'native-transcripts', startMs: payload.startMs, endMs: payload.endMs, totals: combined, complete: true, listingLimited: false,
        filesChecked: 2, filesTotal: 2, bytesRead: 0, duplicates: 0, unidentifiedRecords: 0, providers,
        models: ['codex-cli', 'claude-cli'].map(providerId => ({ ...total, providerId, modelId: 'test-model', speed: 'standard' })),
        days: [{ ...combined, startMs }], hours: [], providerDays: ['codex-cli', 'claude-cli'].map(providerId => ({ ...total, startMs, providerId })), providerHours: [] }))
    }
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsagePage onClose={() => {}} />))
      act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Tokens')!.click())
      expect(host.textContent).toContain('2 sessions')
      expect(host.querySelectorAll('[aria-label^="Inspect"]')).toHaveLength(2)
      const chart = () => host.querySelector('[role="img"]')!
      expect(chart().querySelectorAll('[title="Codex · 17"], [title="Claude · 17"]')).toHaveLength(2)
      const codex = host.querySelector<HTMLButtonElement>('[aria-label="Include Codex usage"]')!
      act(() => codex.click())
      expect(codex.getAttribute('aria-pressed')).toBe('false')
      expect(host.textContent).toContain('1 sessions')
      expect(host.querySelector('[aria-label="Inspect Codex test-model standard"]')).toBeNull()
      expect(chart().querySelector('[title="Codex · 17"]')).toBeNull()
      expect(chart().querySelector('[title="Claude · 17"]')).not.toBeNull()
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Include Claude usage"]')!.click())
      expect(host.textContent).toContain('0 sessions')
      expect(chart().querySelectorAll('[title="Claude · 17"]')).toHaveLength(0)
      expect(requests).toBe(1)
      act(() => codex.click())
      expect(host.querySelectorAll('[aria-label^="Inspect"]')).toHaveLength(1)
      expect(chart().querySelector('[title="Codex · 17"]')).not.toBeNull()
    } finally { act(() => root.unmount()) }
  })
  it('rejects malformed saved prices without treating missing fields as zero', () => {
    const price = { providerId: 'codex-cli', modelId: 'test-model', speed: 'standard', inputPerMillion: 0, cachedInputPerMillion: null, cacheWritePerMillion: null, outputPerMillion: 1 }
    expect(parseUsagePrices([price])).toEqual([price])
    for (const value of [{}, [price, price], [{ ...price, inputPerMillion: -1 }], [{ ...price, outputPerMillion: Infinity }], [{ ...price, cachedInputPerMillion: undefined }], [{ ...price, modelId: '' }], [{ ...price, providerId: 'unknown' }]]) expect(parseUsagePrices(value)).toEqual([])
  })
  it('keeps native host errors visible and offers a retry instead of displaying zero usage', async () => {
    storage()
    window.cefQuery = request => request.onFailure(400, 'Usage history is not available on remote hosts yet.')
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsagePage onClose={() => {}} />))
      expect(host.querySelector('[role="alert"]')?.textContent).toContain('remote hosts')
      expect(host.textContent).toContain('Retry')
      expect(host.textContent).not.toContain('$0.00')
    } finally { act(() => root.unmount()) }
  })
})

describe('UsageLimits', () => {
  it.each(['gemini-cli', 'opencode-cli'])('labels %s attachment totals separately from complete native-session totals', providerId => {
    const previous = useAppStore.getState()
    const counts = { inputTokens: 100, cachedInputTokens: 70, cacheWriteInputTokens: 0, outputTokens: 25, reasoningOutputTokens: 5, totalTokens: 125 }
    useAppStore.setState({
      sessions: [{ id: 'gemini', name: 'Resumed chat', providerId }] as typeof previous.sessions,
      acpBindingBySessionId: { gemini: { providerId, providerUsage: { tokenUsage: { updatedAt: Date.now(), total: counts, last: counts, modelContextWindow: null }, rateLimits: null } } } as typeof previous.acpBindingBySessionId,
    })
    const host = document.createElement('div'), root = createRoot(host)
    try {
      act(() => root.render(<UsageLimits executionHostId="local" />))
      expect(host.textContent).toContain('Tokens observed while attached to Resumed chat: 125')
      expect(host.textContent).toContain('Input 100 · Output 25')
      expect(host.textContent).toContain('Of input: cache read 70 · cache write 0')
      expect(host.textContent).toContain('Of output: reasoning 5')
      expect(host.textContent).toContain('Last turn: 125 tokens')
      expect(host.textContent).not.toContain('Tokens consumed in Resumed chat')
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
  })

  it('keeps reported zero and known last-turn tokens while labeling an invalid update time', () => {
    const previous = useAppStore.getState()
    const zero = { inputTokens: 0, cachedInputTokens: 0, cacheWriteInputTokens: 0, outputTokens: 0, reasoningOutputTokens: 0, totalTokens: 0 }
    useAppStore.setState({
      sessions: [{ id: 'codex', name: 'Zero usage', providerId: 'codex-cli' }] as typeof previous.sessions,
      acpBindingBySessionId: { codex: { providerId: 'codex-cli', providerUsage: { tokenUsage: { updatedAt: Date.now() + 60000, total: zero, last: zero, modelContextWindow: null }, rateLimits: null } } } as typeof previous.acpBindingBySessionId,
    })
    const host = document.createElement('div'), root = createRoot(host)
    try {
      act(() => root.render(<UsageLimits executionHostId="local" />))
      expect(host.textContent).toContain('Tokens consumed in Zero usage: 0')
      expect(host.textContent).toContain('Last turn: 0 tokens')
      expect(host.textContent).toContain('Update time unavailable')
      expect(host.textContent).toContain('Cached snapshot')
      expect(host.textContent).toContain('Consumed tokens unavailable.')
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
  })

  it('shows provider consumption from the latest matching local chat without summing snapshots or cache subsets', () => {
    const previous = useAppStore.getState()
    const tokens = (time: number, count: number) => ({ updatedAt: time, total: { inputTokens: 120, cachedInputTokens: 20, cacheWriteInputTokens: 5, outputTokens: 41, reasoningOutputTokens: 3, totalTokens: count }, last: null, modelContextWindow: null })
    useAppStore.setState({
      sessions: [{ id: 'old', name: 'Old chat', providerId: 'claude-cli' }, { id: 'new', name: 'Current Claude', providerId: 'claude-cli' }, { id: 'remote', name: 'Remote chat', providerId: 'claude-cli', executionHostId: 'ssh' }, { id: 'mismatch', name: 'Wrong provider', providerId: 'opencode-cli' }] as typeof previous.sessions,
      acpBindingBySessionId: {
        old: { providerId: 'claude-cli', providerUsage: { tokenUsage: tokens(1, 99), rateLimits: null } },
        new: { providerId: 'claude-cli', providerUsage: { tokenUsage: tokens(2, 161), rateLimits: null } },
        remote: { providerId: 'claude-cli', providerUsage: { tokenUsage: tokens(3, 777), rateLimits: null } },
        mismatch: { providerId: 'claude-cli', providerUsage: { tokenUsage: tokens(4, 888), rateLimits: null } },
      } as typeof previous.acpBindingBySessionId,
    })
    const host = document.createElement('div'), root = createRoot(host)
    try {
      act(() => root.render(<UsageLimits executionHostId="local" />))
      expect(host.textContent).toContain('Tokens consumed in Current Claude: 161')
      expect(host.textContent).toContain('Input 120 · Output 41')
      expect(host.textContent).toContain('Of input: cache read 20 · cache write 5')
      expect(host.textContent).toContain('Of output: reasoning 3')
      expect(host.textContent).toContain('Last turn: Unavailable')
      expect(host.textContent).toContain('Cached snapshot')
      expect(host.textContent).not.toContain('Tokens consumed in Old chat')
      expect(host.textContent).not.toContain('Remote chat')
      expect(host.textContent).not.toContain('Wrong provider')
      expect(host.textContent).not.toContain('Tokens consumed in Current Claude: 186')
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
  })

  it('keeps machine allowances separate in the all-machines view', () => {
    const previous = useAppStore.getState()
    const limits = (percent: number) => ({ updatedAt: Date.now(), limitId: '', limitName: '', planType: '', primary: { usedPercent: percent, resetsAt: null, windowDurationMinutes: 300 }, secondary: null, credits: null, individualLimit: null, spendControlReached: null, rateLimitReachedType: '' })
    useAppStore.setState({ executionHosts: [{ id: 'local', label: 'Mac' }, { id: 'peer', label: 'Windows' }] as typeof previous.executionHosts,
      sessions: [{ id: 'local-chat', providerId: 'codex-cli', executionHostId: 'local' }, { id: 'remote-chat', providerId: 'codex-cli', executionHostId: 'peer' }] as typeof previous.sessions,
      acpBindingBySessionId: { 'local-chat': { providerId: 'codex-cli', providerUsage: { tokenUsage: null, rateLimits: limits(25) } }, 'remote-chat': { providerId: 'codex-cli', providerUsage: { tokenUsage: null, rateLimits: limits(90) } } } as typeof previous.acpBindingBySessionId })
    const host = document.createElement('div'), root = createRoot(host)
    try {
      act(() => root.render(<UsageLimits executionHostId="*" />))
      expect(host.querySelector('[aria-label="Mac limits"]')?.textContent).toContain('75% remaining')
      expect(host.querySelector('[aria-label="Windows limits"]')?.textContent).toContain('10% remaining')
      expect(host.querySelector('[aria-label="Mac limits"]')?.textContent).not.toContain('10% remaining')
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
  })

  it('uses the newest matching provider/host snapshot and labels passed resets', () => {
    const previous = useAppStore.getState()
    const limits = (percent: number, time: number) => ({ updatedAt: time, limitId: '', limitName: '', planType: '', primary: { usedPercent: percent, resetsAt: Date.now() / 1000 - 60, windowDurationMinutes: 300 }, secondary: null, credits: null, individualLimit: null, spendControlReached: null, rateLimitReachedType: '' })
    useAppStore.setState({ sessions: [{ id: 'older', providerId: 'codex-cli' }, { id: 'newer', name: 'Latest chat', providerId: 'codex-cli' }, { id: 'remote', providerId: 'codex-cli', executionHostId: 'ssh' }, { id: 'mismatch', providerId: 'claude-cli' }] as typeof previous.sessions,
      acpBindingBySessionId: { older: { providerId: 'codex-cli', providerUsage: { tokenUsage: null, rateLimits: limits(90, 1) } }, newer: { providerId: 'codex-cli', providerUsage: { tokenUsage: null, contextUsage: { updatedAt: 2, usedTokens: 1234, windowTokens: 200000 }, rateLimits: limits(25, 2) } }, remote: { providerId: 'codex-cli', providerUsage: { tokenUsage: null, rateLimits: limits(10, 3) } }, mismatch: { providerId: 'codex-cli', providerUsage: { tokenUsage: null, rateLimits: limits(5, 4) } } } as typeof previous.acpBindingBySessionId })
    const host = document.createElement('div'), root = createRoot(host)
    try {
      act(() => root.render(<UsageLimits executionHostId="local" />))
      expect(host.textContent).toContain('75% remaining')
      expect(host.textContent).toContain('Context in Latest chat: 1,234 of 200,000 tokens')
      expect(host.textContent).not.toContain('90% remaining')
      expect(host.textContent).not.toContain('95% remaining')
      expect(host.textContent).toContain('reset passed, awaiting update')
      expect(host.querySelector('[role="meter"]')?.getAttribute('aria-valuenow')).toBe('25')
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
  })
})
