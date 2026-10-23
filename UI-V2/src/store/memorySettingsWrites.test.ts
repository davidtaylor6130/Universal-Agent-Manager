import { describe, expect, it, vi } from 'vitest'
import {
  createMemorySettingsWrites, parseMemorySettingsResult, parseMemorySettingsSnapshot,
  type MemorySettingsValues, type MemorySettingsWriteDependencies,
} from './memorySettingsWrites'

const scope = '11111111-1111-4111-8111-111111111111'
const otherScope = '22222222-2222-4222-8222-222222222222'
const base = (): MemorySettingsValues => ({
  memoryEnabledDefault: true, memoryLevelDefault: 'strict', memoryIdleDelaySeconds: 60,
  memoryRecallBudgetBytes: 2048, goalMaxLoopIterations: 200, acpSetupInactivityTimeoutSeconds: 600,
  acpTurnOutputLimitMiB: 1024, memoryWorkerBindings: {}, permissionReviewerProviderId: '', permissionReviewerModelId: '',
})
type Request = Parameters<MemorySettingsWriteDependencies['request']>[0]
type Response = Awaited<ReturnType<MemorySettingsWriteDependencies['request']>>
function fixture(native = true) {
  let displayed = base(), primary = base(), revision = 0, id = 0
  const calls: Request[] = []
  const available: Array<{ request: Request; resolve: (response: Response) => void }> = []
  let waiting: ((value: typeof available[number]) => void) | undefined
  const failures: string[] = []
  const publishedFields: string[][] = []
  let publishError = false, applyError = false
  let readOverride: Response | Promise<Response> | undefined
  const request = vi.fn((call: Request): Promise<Response> => {
    calls.push(call)
    if (call.action === 'getMemorySettings') return Promise.resolve(readOverride ?? { ok: true, data: { scope, revision, primaryValidated: true, values: primary } })
    return new Promise((resolve) => {
      const item = { request: call, resolve }
      if (waiting) { const notify = waiting; waiting = undefined; notify(item) } else available.push(item)
    })
  })
  const controller = createMemorySettingsWrites({
    readValues: () => displayed,
    applyValues: (values) => { if (applyError) throw new Error('display publication failed'); displayed = { ...displayed, ...values } },
    isNative: () => native, request, requestId: () => `memory-action-${++id}`,
    reportFailure: (outcome) => { failures.push(outcome) },
    publishUnconfirmedFields: (fields) => { publishedFields.push([...fields]); if (publishError) throw new Error('display observer failed') },
  })
  const nextSet = () => available.length ? Promise.resolve(available.shift()!) : new Promise<typeof available[number]>((resolve) => { waiting = resolve })
  function reply(item: typeof available[number], outcome: 'committed' | 'rejected' | 'unknown', values = primary, nextRevision = revision) {
    if (outcome === 'committed') { primary = values; revision = nextRevision }
    item.resolve({ ok: true, data: { memoryRequestId: item.request.payload.memoryRequestId, outcome, scope, revision: nextRevision, primaryValidated: true, values } })
  }
  return {
    controller, calls, request, failures, nextSet, reply, publishedFields,
    setPublishError: (value: boolean) => { publishError = value },
    setApplyError: (value: boolean) => { applyError = value },
    get unconfirmedFields() { return publishedFields[publishedFields.length - 1] ?? [] },
    get displayed() { return displayed }, get primary() { return primary },
    setReadResponse: (response: Response | Promise<Response>) => { readOverride = response },
    applyProjection: (values: Partial<MemorySettingsValues>) => { displayed = { ...displayed, ...controller.overlay(values) } },
  }
}

describe('memory settings action-local write ownership', () => {
  it('uses existing clamps and sends only explicitly requested fields', async () => {
    const f = fixture()
    const save = f.controller.save({ memoryIdleDelaySeconds: -1, memoryRecallBudgetBytes: 999999, goalMaxLoopIterations: -1, acpSetupInactivityTimeoutSeconds: 999999, acpTurnOutputLimitMiB: 999999 })
    expect(f.displayed).toMatchObject({ memoryIdleDelaySeconds: 30, memoryRecallBudgetBytes: 8192, goalMaxLoopIterations: 200, acpSetupInactivityTimeoutSeconds: 3600, acpTurnOutputLimitMiB: 4096 })
    const call = await f.nextSet()
    expect(call.request.payload).toEqual({ expectedSettingsScope: scope, expectedSettingsRevision: 0, memoryRequestId: call.request.requestId, idleDelaySeconds: 30, recallBudgetBytes: 8192, goalMaxLoopIterations: 200, acpSetupInactivityTimeoutSeconds: 3600, acpTurnOutputLimitMiB: 4096 })
    f.reply(call, 'committed', f.displayed, 1)
    expect(await save).toBe(true)
  })

  it('restores validated60 after both60to90to120 requests are rejected', async () => {
    const f = fixture()
    const a = f.controller.save({ memoryIdleDelaySeconds: 90 })
    const b = f.controller.save({ memoryIdleDelaySeconds: 120 })
    expect(f.displayed.memoryIdleDelaySeconds).toBe(120)
    const first = await f.nextSet()
    expect(f.calls.map((request) => request.action)).toEqual(['getMemorySettings', 'setMemorySettings'])
    f.reply(first, 'rejected', base())
    expect(await a).toBe(false)
    expect(f.displayed.memoryIdleDelaySeconds).toBe(120)
    const second = await f.nextSet()
    f.reply(second, 'rejected', base())
    expect(await b).toBe(false)
    expect(f.displayed.memoryIdleDelaySeconds).toBe(60)
    expect(f.failures).toEqual(['rejected', 'rejected'])
  })

  it('keeps canonical90 when A commits and B is rejected against90', async () => {
    const f = fixture()
    const a = f.controller.save({ memoryIdleDelaySeconds: 90 })
    const b = f.controller.save({ memoryIdleDelaySeconds: 120 })
    f.reply(await f.nextSet(), 'committed', { ...base(), memoryIdleDelaySeconds: 90 }, 1)
    expect(await a).toBe(true)
    const second = await f.nextSet()
    expect(second.request.payload.expectedSettingsRevision).toBe(1)
    f.reply(second, 'rejected', { ...base(), memoryIdleDelaySeconds: 90 }, 1)
    expect(await b).toBe(false)
    expect(f.displayed.memoryIdleDelaySeconds).toBe(90)
  })

  it('accepts B120 after A rejection without using the optimistic90 predecessor', async () => {
    const f = fixture()
    const a = f.controller.save({ memoryIdleDelaySeconds: 90 })
    const b = f.controller.save({ memoryIdleDelaySeconds: 120 })
    f.reply(await f.nextSet(), 'rejected', base())
    expect(await a).toBe(false)
    f.reply(await f.nextSet(), 'committed', { ...base(), memoryIdleDelaySeconds: 120 }, 1)
    expect(await b).toBe(true)
    expect(f.displayed.memoryIdleDelaySeconds).toBe(120)
  })

  it('retains an unknown draft through pending reads then settles it from a later committed whole-file receipt', async () => {
    const f = fixture()
    const a = f.controller.save({ memoryIdleDelaySeconds: 90 })
    f.reply(await f.nextSet(), 'unknown', { ...base(), memoryIdleDelaySeconds: 90 }, 1)
    expect(await a).toBe(false)
    const b = f.controller.save({ memoryRecallBudgetBytes: 4096 })
    const second = await f.nextSet()
    expect(second.request.payload).not.toHaveProperty('idleDelaySeconds')
    expect(f.displayed.memoryIdleDelaySeconds).toBe(90)
    f.reply(second, 'committed', { ...base(), memoryRecallBudgetBytes: 4096 }, 1)
    expect(await b).toBe(true)
    expect(f.displayed).toMatchObject({ memoryIdleDelaySeconds: 60, memoryRecallBudgetBytes: 4096 })
  })

  it('preserves a newer same-field intent while older canonical normalization arrives', async () => {
    const f = fixture()
    const a = f.controller.save({ memoryIdleDelaySeconds: 90 })
    const b = f.controller.save({ memoryIdleDelaySeconds: 120 })
    f.reply(await f.nextSet(), 'committed', { ...base(), memoryIdleDelaySeconds: 80 }, 1)
    expect(await a).toBe(true)
    expect(f.displayed.memoryIdleDelaySeconds).toBe(120)
    f.reply(await f.nextSet(), 'committed', { ...base(), memoryIdleDelaySeconds: 120 }, 2)
    expect(await b).toBe(true)
  })

  it('overlays pending and unknown drafts on full and partial native projections without touching host defaults', async () => {
    const f = fixture()
    const save = f.controller.save({ memoryIdleDelaySeconds: 90 })
    const hosts = { ssh: { defaultNewChatProviderId: 'opencode-cli' } }
    const incoming = { ...base(), hostScopedSettingsValues: hosts, stateRevision: 987 }
    expect(f.controller.overlay(incoming)).toMatchObject({ memoryIdleDelaySeconds: 90, stateRevision: 987 })
    expect(f.controller.overlay(incoming).hostScopedSettingsValues).toBe(hosts)
    f.reply(await f.nextSet(), 'unknown')
    expect(await save).toBe(false)
    expect(f.controller.overlay({ memoryIdleDelaySeconds: 60 }).memoryIdleDelaySeconds).toBe(90)
  })

  it('treats enabled and level as one atomically superseded pair', async () => {
    const f = fixture()
    const a = f.controller.save({ memoryEnabledDefault: false })
    const b = f.controller.save({ memoryLevelDefault: 'open' })
    const first = await f.nextSet()
    expect(first.request.payload).toMatchObject({ enabledDefault: false, levelDefault: 'off' })
    f.reply(first, 'rejected', base())
    expect(await a).toBe(false)
    expect(f.displayed).toMatchObject({ memoryEnabledDefault: true, memoryLevelDefault: 'open' })
    const second = await f.nextSet()
    expect(second.request.payload).toMatchObject({ enabledDefault: true, levelDefault: 'open' })
    f.reply(second, 'committed', { ...base(), memoryLevelDefault: 'open' }, 1)
    expect(await b).toBe(true)
  })

  it('rejects malformed canonical values, wrong identities and unproven publication ordering', () => {
    const expected = { memoryRequestId: 'owned', scope, revision: 4 }
    const valid = { memoryRequestId: 'owned', outcome: 'committed', scope, revision: 5, primaryValidated: true, values: base() }
    expect(parseMemorySettingsResult(valid, expected)?.outcome).toBe('committed')
    for (const invalid of [
      {}, { ...valid, memoryRequestId: 'other' }, { ...valid, scope: otherScope }, { ...valid, revision: 4 },
      { ...valid, revision: -1 }, { ...valid, primaryValidated: false },
      { ...valid, values: { ...base(), memoryEnabledDefault: false } },
      { ...valid, values: { ...base(), memoryIdleDelaySeconds: 29 } },
      { ...valid, values: { ...base(), goalMaxLoopIterations: 0 } },
      { ...valid, values: { ...base(), allSettings: 'must not become authority' } },
      { ...valid, outcome: 'rejected', revision: 3 },
    ]) expect(parseMemorySettingsResult(invalid, expected)).toBeNull()
    expect(parseMemorySettingsResult({ ...valid, outcome: 'unknown' }, expected)).toMatchObject({ outcome: 'unknown', primaryValidated: false, values: undefined })
  })

  it('does not SET after a failed or unvalidated bounded fresh read', async () => {
    for (const response of [{ ok: false, error: 'lost' }, { ok: true, data: {} }, { ok: true, data: { scope, revision: 0, primaryValidated: false } }]) {
      const f = fixture(); f.setReadResponse(response)
      expect(await f.controller.save({ memoryIdleDelaySeconds: 90 })).toBe(false)
      expect(f.calls.map((call) => call.action)).toEqual(['getMemorySettings'])
      expect(f.displayed.memoryIdleDelaySeconds).toBe(60)
      expect(f.failures).toEqual(['not-sent'])
    }
    expect(parseMemorySettingsSnapshot({ scope, revision: 0, primaryValidated: true })).toBeNull()
  })

  it('never rolls back or automatically retries generic transport failure or legacy success', async () => {
    for (const response of [{ ok: false, error: 'may have committed' }, { ok: true, data: {} }, { ok: true, data: 'legacy' }]) {
      const f = fixture()
      const save = f.controller.save({ memoryIdleDelaySeconds: 90 })
      ;(await f.nextSet()).resolve(response)
      expect(await save).toBe(false)
      expect(f.displayed.memoryIdleDelaySeconds).toBe(90)
      expect(f.calls.map((call) => call.action)).toEqual(['getMemorySettings', 'setMemorySettings'])
      expect(f.failures).toEqual(['unknown'])
    }
  })

  it('sends only changed worker entries then settles older unknown entries from committed canonical values', async () => {
    const f = fixture()
    const oldBinding = { workerProviderId: 'codex-cli', workerModelId: 'draft-model' }
    const newBinding = { workerProviderId: 'claude-cli', workerModelId: 'chosen-model' }
    const a = f.controller.save({ memoryWorkerBindings: { 'opencode-cli': oldBinding } })
    f.reply(await f.nextSet(), 'unknown')
    expect(await a).toBe(false)
    const b = f.controller.save({ memoryWorkerBindings: { ...f.displayed.memoryWorkerBindings, 'gemini-cli': newBinding } })
    const second = await f.nextSet()
    expect(second.request.payload.workerBindings).toEqual({ 'gemini-cli': newBinding })
    expect(f.displayed.memoryWorkerBindings).toEqual({ 'opencode-cli': oldBinding, 'gemini-cli': newBinding })
    f.reply(second, 'committed', { ...base(), memoryWorkerBindings: { 'gemini-cli': newBinding } }, 1)
    expect(await b).toBe(true)
    expect(f.displayed.memoryWorkerBindings).toEqual({ 'gemini-cli': newBinding })
  })

  it('keeps ownership and late callbacks isolated between actual controller instances', async () => {
    const a = fixture(), b = fixture()
    const saveA = a.controller.save({ memoryIdleDelaySeconds: 90 })
    const saveB = b.controller.save({ memoryIdleDelaySeconds: 120 })
    b.reply(await b.nextSet(), 'committed', { ...base(), memoryIdleDelaySeconds: 120 }, 1)
    expect(await saveB).toBe(true)
    a.reply(await a.nextSet(), 'rejected', base())
    expect(await saveA).toBe(false)
    expect(a.displayed.memoryIdleDelaySeconds).toBe(60)
    expect(b.displayed.memoryIdleDelaySeconds).toBe(120)
  })

  it('applies browser settings locally without manufacturing native receipts or requests', async () => {
    const f = fixture(false)
    expect(await f.controller.save({ memoryEnabledDefault: false, memoryIdleDelaySeconds: -1 })).toBe(true)
    expect(f.displayed).toMatchObject({ memoryEnabledDefault: false, memoryLevelDefault: 'off', memoryIdleDelaySeconds: 30 })
    expect(f.calls).toEqual([])
    expect(f.failures).toEqual([])
  })
  it('settles older Unknown A on B commit while newer queued C remains overlaid and cannot resurrect A', async () => {
    const f = fixture()
    const a = f.controller.save({ memoryIdleDelaySeconds: 90 })
    f.reply(await f.nextSet(), 'unknown')
    expect(await a).toBe(false)
    const b = f.controller.save({ memoryRecallBudgetBytes: 4096 })
    const second = await f.nextSet()
    expect(f.displayed.memoryIdleDelaySeconds).toBe(90)
    const c = f.controller.save({ memoryIdleDelaySeconds: 120 })
    let releaseRead!: (response: Response) => void
    f.setReadResponse(new Promise<Response>((resolve) => { releaseRead = resolve }))
    f.reply(second, 'committed', { ...base(), memoryRecallBudgetBytes: 4096 }, 1)
    expect(await b).toBe(true)
    expect(f.displayed).toMatchObject({ memoryIdleDelaySeconds: 120, memoryRecallBudgetBytes: 4096 })
    expect(f.controller.overlay({ memoryIdleDelaySeconds: 60 }).memoryIdleDelaySeconds).toBe(120)
    releaseRead({ ok: false, error: 'C read did not arrive' })
    // C has no publication; removing it reveals B's complete canonical base,
    // never the already retired Unknown A or C's optimistic predecessor.
    expect(await c).toBe(false)
    expect(f.displayed).toMatchObject({ memoryIdleDelaySeconds: 60, memoryRecallBudgetBytes: 4096 })
    expect(f.failures).toEqual(['unknown', 'not-sent'])
    expect(f.calls.filter((call) => call.action === 'setMemorySettings')).toHaveLength(2)
  })

  it('restores60 rather than optimistic90 when both queued reads fail before either SET', async () => {
    const f = fixture()
    f.setReadResponse({ ok: false, error: 'read unavailable' })
    const a = f.controller.save({ memoryIdleDelaySeconds: 90 })
    const b = f.controller.save({ memoryIdleDelaySeconds: 120 })
    expect(f.displayed.memoryIdleDelaySeconds).toBe(120)
    expect(await a).toBe(false)
    expect(await b).toBe(false)
    expect(f.displayed.memoryIdleDelaySeconds).toBe(60)
    expect(f.failures).toEqual(['not-sent', 'not-sent'])
    expect(f.calls.map((call) => call.action)).toEqual(['getMemorySettings', 'getMemorySettings'])
  })

  it('restores an older truly Unknown draft when newer same-field intent never dispatched', async () => {
    const f = fixture()
    const a = f.controller.save({ memoryIdleDelaySeconds: 90 })
    f.reply(await f.nextSet(), 'unknown')
    expect(await a).toBe(false)
    f.setReadResponse({ ok: true, data: { scope, revision: 0, primaryValidated: false } })
    expect(await f.controller.save({ memoryIdleDelaySeconds: 120 })).toBe(false)
    expect(f.displayed.memoryIdleDelaySeconds).toBe(90)
    expect(f.controller.overlay({ memoryIdleDelaySeconds: 60 }).memoryIdleDelaySeconds).toBe(90)
    expect(f.failures).toEqual(['unknown', 'not-sent'])
    expect(f.calls.filter((call) => call.action === 'setMemorySettings')).toHaveLength(1)
  })

  it('does not resurrect a rejected predecessor after later same-field GET failure', async () => {
    const f = fixture()
    const a = f.controller.save({ memoryIdleDelaySeconds: 90 })
    const b = f.controller.save({ memoryIdleDelaySeconds: 120 })
    const first = await f.nextSet()
    f.setReadResponse({ ok: false, error: 'B read failed' })
    f.reply(first, 'rejected', base())
    expect(await a).toBe(false)
    expect(await b).toBe(false)
    expect(f.displayed.memoryIdleDelaySeconds).toBe(60)
    expect(f.failures).toEqual(['rejected', 'not-sent'])
  })

  it('cannot use a generic later response to settle an earlier Unknown field', async () => {
    const f = fixture()
    const a = f.controller.save({ memoryIdleDelaySeconds: 90 })
    f.reply(await f.nextSet(), 'unknown')
    expect(await a).toBe(false)
    const b = f.controller.save({ memoryRecallBudgetBytes: 4096 })
    ;(await f.nextSet()).resolve({ ok: true, data: {} })
    expect(await b).toBe(false)
    expect(f.displayed).toMatchObject({ memoryIdleDelaySeconds: 90, memoryRecallBudgetBytes: 4096 })
    expect(f.failures).toEqual(['unknown', 'unknown'])
  })

  it('classifies a throwing GET transport as not-sent and leaves newer unrelated display fields intact', async () => {
    const f = fixture()
    f.request.mockImplementationOnce(() => Promise.reject(new Error('read connection lost')))
    const save = f.controller.save({ memoryIdleDelaySeconds: 90 })
    f.applyProjection({ memoryRecallBudgetBytes: 4096 })
    expect(await save).toBe(false)
    expect(f.displayed).toMatchObject({ memoryIdleDelaySeconds: 60, memoryRecallBudgetBytes: 4096 })
    expect(f.failures).toEqual(['not-sent'])
    expect(f.request).toHaveBeenCalledTimes(1)
    expect(f.request.mock.calls[0][0].action).toBe('getMemorySettings')
  })

})


describe('memory settings controller bootstrap', () => {
  it('waits for an initialized store before reading the first worker draft baseline', async () => {
    let displayed: MemorySettingsValues | undefined
    const readValues = vi.fn(() => {
      if (!displayed) throw new Error('Zustand state is not initialized yet')
      return displayed
    })
    const failures: string[] = []
    let id = 0
    const request = vi.fn(async (_request: Parameters<MemorySettingsWriteDependencies['request']>[0]) => ({ ok: false, error: 'read unavailable; no SET was sent' }))
    const controller = createMemorySettingsWrites({
      readValues, applyValues: (values) => { displayed = { ...displayed!, ...values } },
      isNative: () => true, request, requestId: () => `bootstrap-${++id}`,
      reportFailure: (outcome) => failures.push(outcome),
    })
    expect(readValues).not.toHaveBeenCalled()
    displayed = base()
    await expect(controller.save({ memoryWorkerBindings: { 'codex-cli': { workerProviderId: 'claude-cli', workerModelId: 'chosen' } } })).resolves.toBe(false)
    expect(displayed.memoryWorkerBindings).toEqual({})
    expect(request).toHaveBeenCalledTimes(1)
    expect(request.mock.calls[0][0]).toMatchObject({ action: 'getMemorySettings' })
    expect(failures).toEqual(['not-sent'])
  })
})

describe('persistent unconfirmed memory field metadata', () => {
  it('keeps an exact Unknown field label through a generic native settings projection', async () => {
    const f = fixture()
    const save = f.controller.save({ memoryIdleDelaySeconds: 90 })
    expect(f.unconfirmedFields).toEqual([])
    f.reply(await f.nextSet(), 'unknown')
    expect(await save).toBe(false)
    expect(f.unconfirmedFields).toEqual(['memoryIdleDelaySeconds'])
    const publications = f.publishedFields.length
    f.applyProjection({ ...base(), memoryRecallBudgetBytes: 4096 })
    expect(f.displayed).toMatchObject({ memoryIdleDelaySeconds: 90, memoryRecallBudgetBytes: 4096 })
    expect(f.unconfirmedFields).toEqual(['memoryIdleDelaySeconds'])
    expect(f.publishedFields).toHaveLength(publications)
  })

  it('clears old Unknown metadata only after a typed committed full canonical receipt', async () => {
    const f = fixture()
    const a = f.controller.save({ memoryIdleDelaySeconds: 90 })
    f.reply(await f.nextSet(), 'unknown')
    expect(await a).toBe(false)
    const b = f.controller.save({ memoryRecallBudgetBytes: 4096 })
    const second = await f.nextSet()
    expect(f.unconfirmedFields).toEqual(['memoryIdleDelaySeconds'])
    f.reply(second, 'committed', { ...base(), memoryRecallBudgetBytes: 4096 }, 1)
    expect(await b).toBe(true)
    expect(f.unconfirmedFields).toEqual([])
    expect(f.displayed.memoryIdleDelaySeconds).toBe(60)
  })

  it('hides an older Unknown label while a newer same-field owner is pending and reveals it after not-sent', async () => {
    const f = fixture()
    const a = f.controller.save({ memoryIdleDelaySeconds: 90 })
    f.reply(await f.nextSet(), 'unknown')
    expect(await a).toBe(false)
    expect(f.unconfirmedFields).toEqual(['memoryIdleDelaySeconds'])
    let releaseRead!: (response: Response) => void
    f.setReadResponse(new Promise<Response>((resolve) => { releaseRead = resolve }))
    const b = f.controller.save({ memoryIdleDelaySeconds: 120 })
    expect(f.unconfirmedFields).toEqual([])
    expect(f.displayed.memoryIdleDelaySeconds).toBe(120)
    releaseRead({ ok: false, error: 'B was never dispatched' })
    expect(await b).toBe(false)
    expect(f.displayed.memoryIdleDelaySeconds).toBe(90)
    expect(f.unconfirmedFields).toEqual(['memoryIdleDelaySeconds'])
    expect(f.failures).toEqual(['unknown', 'not-sent'])
  })

  it('deduplicates worker Unknown slots while keeping an independently visible Unknown entry', async () => {
    const f = fixture()
    const binding = { workerProviderId: 'claude-cli', workerModelId: 'chosen' }
    const a = f.controller.save({ memoryWorkerBindings: { 'codex-cli': binding, 'gemini-cli': binding } })
    f.reply(await f.nextSet(), 'unknown')
    expect(await a).toBe(false)
    expect(f.unconfirmedFields).toEqual(['memoryWorkerBindings'])
    const b = f.controller.save({ memoryWorkerBindings: { ...f.displayed.memoryWorkerBindings, 'codex-cli': { ...binding, workerModelId: 'newer' } } })
    // The Codex slot is Pending, but the Gemini slot is still visibly Unknown.
    expect(f.unconfirmedFields).toEqual(['memoryWorkerBindings'])
    f.reply(await f.nextSet(), 'rejected', base())
    expect(await b).toBe(false)
    expect(f.unconfirmedFields).toEqual(['memoryWorkerBindings'])
  })

  it('does not let a throwing unconfirmed observer own failure or committed success', async () => {
    const f = fixture()
    f.setPublishError(true)
    const a = f.controller.save({ memoryIdleDelaySeconds: 90 })
    f.reply(await f.nextSet(), 'unknown')
    expect(await a).toBe(false)
    expect(f.failures).toEqual(['unknown'])
    const b = f.controller.save({ memoryIdleDelaySeconds: 120 })
    f.reply(await f.nextSet(), 'committed', { ...base(), memoryIdleDelaySeconds: 120 }, 1)
    expect(await b).toBe(true)
    expect(f.failures).toEqual(['unknown'])
  })

  it('keeps the native committed result true when only UI value publication throws', async () => {
    const f = fixture()
    const save = f.controller.save({ memoryIdleDelaySeconds: 90 })
    const call = await f.nextSet()
    f.setApplyError(true)
    f.reply(call, 'committed', { ...base(), memoryIdleDelaySeconds: 90 }, 1)
    expect(await save).toBe(true)
    expect(f.failures).toEqual([])
    expect(f.unconfirmedFields).toEqual([])
    expect(f.controller.overlay({ memoryIdleDelaySeconds: 60 }).memoryIdleDelaySeconds).toBe(60)
  })

  it('does not read or publish before store construction finishes', () => {
    const readValues = vi.fn(() => { throw new Error('uninitialized store') })
    const publishUnconfirmedFields = vi.fn(() => { throw new Error('uninitialized observer') })
    createMemorySettingsWrites({ readValues, publishUnconfirmedFields, applyValues: vi.fn(), isNative: () => true, request: vi.fn(), requestId: () => 'unused' })
    expect(readValues).not.toHaveBeenCalled()
    expect(publishUnconfirmedFields).not.toHaveBeenCalled()
  })
})


describe('observed native fallback display without commit authority', () => {
  function holdRead(f: ReturnType<typeof fixture>) {
    let release!: (response: Response) => void
    f.setReadResponse(new Promise<Response>((resolve) => { release = resolve }))
    return (response: Response) => release(response)
  }

  it('restores an explicitly observed native75 after the current GET fails before SET', async () => {
    const f = fixture()
    const release = holdRead(f)
    const save = f.controller.save({ memoryIdleDelaySeconds: 90 })
    f.controller.observeProjection({ memoryIdleDelaySeconds: 75 })
    f.applyProjection({ memoryIdleDelaySeconds: 75 })
    expect(f.displayed.memoryIdleDelaySeconds).toBe(90)
    release({ ok: false, error: 'read failed; no write' })
    expect(await save).toBe(false)
    expect(f.displayed.memoryIdleDelaySeconds).toBe(75)
    expect(f.unconfirmedFields).toEqual([])
    expect(f.failures).toEqual(['not-sent'])
    expect(f.calls.map((call) => call.action)).toEqual(['getMemorySettings'])
  })

  it('does not observe absent native fields or any value passed only through the pure overlay', async () => {
    const f = fixture()
    const release = holdRead(f)
    const save = f.controller.save({ memoryIdleDelaySeconds: 90 })
    // An unrelated accepted frame carries no idle-delay field. Its merged store
    // projection still has the optimistic90, which must not become a baseline.
    f.controller.observeProjection({ memoryRecallBudgetBytes: 4096 })
    f.applyProjection({ memoryIdleDelaySeconds: 90, memoryRecallBudgetBytes: 4096 })
    expect(f.controller.overlay({ memoryIdleDelaySeconds: 75 }).memoryIdleDelaySeconds).toBe(90)
    release({ ok: false, error: 'read failed' })
    expect(await save).toBe(false)
    expect(f.displayed).toMatchObject({ memoryIdleDelaySeconds: 60, memoryRecallBudgetBytes: 4096 })
    expect(f.calls.filter((call) => call.action === 'setMemorySettings')).toHaveLength(0)
  })

  it('restores supplied worker snapshots including an explicitly absent owned slot', async () => {
    const draft = { workerProviderId: 'codex-cli', workerModelId: 'draft' }
    const external = { workerProviderId: 'claude-cli', workerModelId: 'external' }
    const snapshots: MemorySettingsValues['memoryWorkerBindings'][] = [
      { 'codex-cli': external, 'gemini-cli': external },
      { 'gemini-cli': external },
    ]
    for (const observed of snapshots) {
      const f = fixture()
      const release = holdRead(f)
      const save = f.controller.save({ memoryWorkerBindings: { 'codex-cli': draft } })
      f.controller.observeProjection({ memoryWorkerBindings: observed })
      f.applyProjection({ memoryWorkerBindings: observed })
      expect(f.displayed.memoryWorkerBindings['codex-cli']).toEqual(draft)
      expect(f.displayed.memoryWorkerBindings['gemini-cli']).toEqual(external)
      release({ ok: false, error: 'read failed' })
      expect(await save).toBe(false)
      expect(f.displayed.memoryWorkerBindings).toEqual(observed)
      expect(f.calls.filter((call) => call.action === 'setMemorySettings')).toHaveLength(0)
    }
  })

  it('keeps newer pending ownership and reveals older true Unknown after both newer reads fail', async () => {
    const f = fixture()
    const a = f.controller.save({ memoryIdleDelaySeconds: 90 })
    f.reply(await f.nextSet(), 'unknown')
    expect(await a).toBe(false)
    const releaseB = holdRead(f)
    const b = f.controller.save({ memoryIdleDelaySeconds: 120 })
    const c = f.controller.save({ memoryIdleDelaySeconds: 150 })
    const releaseC = holdRead(f)
    f.controller.observeProjection({ memoryIdleDelaySeconds: 75 })
    f.applyProjection({ memoryIdleDelaySeconds: 75 })
    expect(f.displayed.memoryIdleDelaySeconds).toBe(150)
    expect(f.unconfirmedFields).toEqual([])
    releaseB({ ok: false, error: 'B never sent' })
    expect(await b).toBe(false)
    expect(f.displayed.memoryIdleDelaySeconds).toBe(150)
    expect(f.unconfirmedFields).toEqual([])
    releaseC({ ok: false, error: 'C never sent' })
    expect(await c).toBe(false)
    expect(f.displayed.memoryIdleDelaySeconds).toBe(90)
    expect(f.unconfirmedFields).toEqual(['memoryIdleDelaySeconds'])
    expect(f.failures).toEqual(['unknown', 'not-sent', 'not-sent'])
    expect(f.calls.filter((call) => call.action === 'setMemorySettings')).toHaveLength(1)
  })

  it('normalizes observed partial level and enabled pairs without borrowing an optimistic member', async () => {
    const cases: Array<{ supplied: Partial<MemorySettingsValues>; expected: Partial<MemorySettingsValues> }> = [
      { supplied: { memoryEnabledDefault: false }, expected: { memoryEnabledDefault: false, memoryLevelDefault: 'off' } },
      { supplied: { memoryLevelDefault: 'open' }, expected: { memoryEnabledDefault: true, memoryLevelDefault: 'open' } },
      { supplied: { memoryLevelDefault: 'open', memoryEnabledDefault: false }, expected: { memoryEnabledDefault: false, memoryLevelDefault: 'off' } },
    ]
    for (const { supplied, expected } of cases) {
      const f = fixture()
      const release = holdRead(f)
      const save = f.controller.save({ memoryLevelDefault: 'balanced' })
      f.controller.observeProjection(supplied)
      f.applyProjection(supplied)
      expect(f.displayed).toMatchObject({ memoryEnabledDefault: true, memoryLevelDefault: 'balanced' })
      // Internal project() calls also use overlay; it must remain observation-free.
      f.controller.overlay({ memoryEnabledDefault: true, memoryLevelDefault: 'strict' })
      release({ ok: false, error: 'read failed' })
      expect(await save).toBe(false)
      expect(f.displayed).toMatchObject(expected)
      expect(f.calls.filter((call) => call.action === 'setMemorySettings')).toHaveLength(0)
    }
  })
})
