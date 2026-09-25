import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { useAppStore } from '../store/useAppStore'
import { createRequestId, isCefContext, sendToCEF, type CEFResponse } from '../ipc/cefBridge'
import {
  UPDATE_CHECK_INTERVAL_MS,
  availableUpdates,
  availableRemoteHelperUpdates,
  fetchLatestUpdateCatalog,
  readCachedUpdateCatalog,
  type LatestUpdateCatalog,
} from '../services/updateCatalog'

function updateCheckErrorIdentity(message: string): string {
  // ponytail: 64-bit FNV-1a keeps settings bounded; collisions are negligible for UI dismissal state.
  let hash = 0xcbf29ce484222325n
  for (let index = 0; index < message.length; index += 1) {
    hash = BigInt.asUintN(64, (hash ^ BigInt(message.charCodeAt(index))) * 0x100000001b3n)
  }
  return `error:${hash.toString(16).padStart(16, '0')}`
}

export function useUpdateMonitor() {
  const appVersion = useAppStore((state) => state.appVersion)
  const runnerProtocolVersion = useAppStore((state) => state.runnerProtocolVersion)
  const providers = useAppStore((state) => state.providers)
  const versionManager = useAppStore((state) => state.cliVersionManager)
  const executionHosts = useAppStore((state) => state.executionHosts)
  const enabled = useAppStore((state) => state.updateChecksEnabled)
  const lastCheckedAt = useAppStore((state) => state.updateLastCheckedAt)
  const dismissedVersions = useAppStore((state) => state.dismissedUpdateVersions)
  const cefStateHydrated = useAppStore((state) => state.lastAppliedStateRevision >= 0)
  const setUpdateSettings = useAppStore((state) => state.setUpdateSettings)
  const refreshCliProviderVersion = useAppStore((state) => state.refreshCliProviderVersion)
  const applyCliProviderVersion = useAppStore((state) => state.applyCliProviderVersion)
  const providerStates = useMemo(() => [...versionManager.providers, ...(versionManager.remoteProviders ?? [])], [versionManager.providers, versionManager.remoteProviders])
  const [catalog, setCatalog] = useState<LatestUpdateCatalog | null>(() => readCachedUpdateCatalog())
  const [requestChecking, setChecking] = useState(false)
  // Native refresh requests return on admission; pushed probe state tracks completion.
  const checking = requestChecking || providerStates.some((provider) => provider.running && provider.status === 'checking')
  const [error, setError] = useState('')
  const [remoteHelperUpdatingId, setRemoteHelperUpdatingId] = useState('')
  const autoCheckAttemptedRef = useRef(false)
  const checkingRef = useRef(false)

  const checkNow = useCallback(async () => {
    if (checkingRef.current) return
    const currentManager = useAppStore.getState().cliVersionManager
    if ([...currentManager.providers, ...(currentManager.remoteProviders ?? [])].some((provider) => provider.running && provider.status === 'checking')) return
    checkingRef.current = true
    setChecking(true)
    setError('')
    try {
      const nextCatalog = await fetchLatestUpdateCatalog()
      setCatalog(nextCatalog)
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'refreshAllCliProviderVersions',
          requestId: createRequestId('refreshAllCliProviderVersions'),
        })
        if (!response.ok) throw new Error(response.error || 'Provider version refresh failed.')
      } else {
        await Promise.all([...versionManager.providers, ...(versionManager.remoteProviders ?? [])].map((provider) => refreshCliProviderVersion(provider.providerId, ...provider.executionHostId ? [provider.executionHostId] : [])))
      }
      if (!await setUpdateSettings({ updateLastCheckedAt: nextCatalog.checkedAt })) {
        throw new Error('The update check completed, but its status could not be saved.')
      }
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'Update check failed.')
    } finally {
      checkingRef.current = false
      setChecking(false)
    }
  }, [refreshCliProviderVersion, setUpdateSettings, versionManager.providers, versionManager.remoteProviders])

  useEffect(() => {
    if (isCefContext() && !cefStateHydrated) return
    if (!enabled) {
      autoCheckAttemptedRef.current = false
      return
    }
    const checkedAt = Date.parse(lastCheckedAt)
    const delay = Number.isFinite(checkedAt)
      ? Math.max(0, UPDATE_CHECK_INTERVAL_MS - (Date.now() - checkedAt))
      : 0
    if (delay > 0) {
      autoCheckAttemptedRef.current = false
      const timer = window.setTimeout(() => {
        if (autoCheckAttemptedRef.current) return
        autoCheckAttemptedRef.current = true
        void checkNow()
      }, delay)
      return () => window.clearTimeout(timer)
    }
    if (!checking && !autoCheckAttemptedRef.current) {
      autoCheckAttemptedRef.current = true
      void checkNow()
    }
  }, [cefStateHydrated, checkNow, checking, enabled, lastCheckedAt])

  const updates = useMemo(() => [
    ...availableUpdates(catalog, appVersion, versionManager, providers, dismissedVersions),
    ...availableRemoteHelperUpdates(appVersion, runnerProtocolVersion, executionHosts, dismissedVersions),
  ], [appVersion, catalog, dismissedVersions, executionHosts, providers, runnerProtocolVersion, versionManager])

  const applyRemoteHelperUpdate = useCallback(async (hostId: string): Promise<CEFResponse> => {
    const host = executionHosts.find((candidate) => candidate.id === hostId)
    if (!host || host.transport !== 'ssh' || remoteHelperUpdatingId) return { ok: false }
    setRemoteHelperUpdatingId(hostId)
    try {
      const response = await sendToCEF({ action: 'installRemoteHost', payload: host })
      if (response.ok) {
        // Native completion can race the pushed state patch; reconcile the card immediately.
        useAppStore.setState((state) => ({
          executionHosts: state.executionHosts.map((candidate) => candidate.id === hostId
            ? { ...candidate, runnerStatus: 'ready', runnerVersion: appVersion, runnerProtocolVersion }
            : candidate),
        }))
      }
      return response
    } catch (error) {
      return { ok: false, error: error instanceof Error ? error.message : undefined }
    } finally {
      setRemoteHelperUpdatingId('')
    }
  }, [appVersion, executionHosts, remoteHelperUpdatingId, runnerProtocolVersion])

  const installCliProviderVersion = useCallback((providerId: string, version: string, executionHostId: string | undefined, signal: AbortSignal) => {
    if (signal.aborted) return Promise.resolve(false)
    return new Promise<boolean>((resolve) => {
      let started = false
      let admitted = false
      let settled = false
      const finish = (succeeded: boolean) => {
        if (settled) return
        settled = true
        unsubscribe()
        window.clearTimeout(timer)
        signal.removeEventListener('abort', abort)
        resolve(succeeded)
      }
      const abort = () => finish(false)
      const observe = () => {
        const manager = useAppStore.getState().cliVersionManager
        const state = [...manager.providers, ...(manager.remoteProviders ?? [])].find((entry) =>
          entry.providerId === providerId && (entry.executionHostId || 'local') === (executionHostId || 'local'))
        if (state?.status === 'installing' && state.selectedVersion === version && state.lastInstallStatus === 'running') started = true
        if (!admitted || !started || state?.running) return
        finish(state?.lastInstallStatus === 'succeeded' && !state.checkError &&
          Boolean(state.installedVersion) && (version === 'latest' || state.installedVersion === version))
      }
      // Subscribe before admission: CEF can push both startup and completion before replying.
      const unsubscribe = useAppStore.subscribe(observe)
      // Native installation is bounded at 15 minutes, followed by a 30-second version probe.
      const timer = window.setTimeout(() => finish(false), 16 * 60 * 1000)
      signal.addEventListener('abort', abort, { once: true })
      void applyCliProviderVersion(providerId, version, executionHostId).then((accepted) => {
        if (!accepted) finish(false)
        else { admitted = true; observe() }
      }, () => finish(false))
    })
  }, [applyCliProviderVersion])

  const dismiss = useCallback((id: string, version: string) => {
    void setUpdateSettings({ dismissedUpdateVersions: { ...dismissedVersions, [id]: version } })
  }, [dismissedVersions, setUpdateSettings])

  const dismissProviderCheckError = useCallback((error: { providerId: string; executionHostId: string; message: string }) => {
    dismiss(JSON.stringify([error.executionHostId, error.providerId]), updateCheckErrorIdentity(error.message))
  }, [dismiss])

  const dismissAll = useCallback(() => {
    void setUpdateSettings({
      dismissedUpdateVersions: {
        ...dismissedVersions,
        ...Object.fromEntries(updates.map((update) => [update.id, update.latestVersion])),
      },
    })
  }, [dismissedVersions, setUpdateSettings, updates])

  const providerUpdateResults = useMemo(() => providerStates.flatMap((state) => {
    if (state.lastInstallStatus !== 'succeeded' && state.lastInstallStatus !== 'failed') return []
    const provider = providers.find((candidate) => candidate.id === state.providerId)
    return [{
      providerId: state.providerId,
      ...(state.executionHostId ? { executionHostId: state.executionHostId } : {}),
      name: `${provider?.shortName || provider?.name || state.providerId}${state.executionHostId ? ` · ${state.executionHostName || state.executionHostId}` : ''}`,
      status: state.lastInstallStatus,
      message: state.message,
      output: state.lastOutput,
      installedVersion: state.installedVersion,
    }]
  }), [providers, providerStates])
  const allProviderCheckErrors = useMemo(() => (versionManager.remoteProviders ?? []).flatMap((state) => {
    const host = executionHosts.find((candidate) => candidate.id === state.executionHostId && candidate.transport === 'ssh')
    if (!host || state.running || !state.checkError) return []
    const provider = providers.find((candidate) => candidate.id === state.providerId)
    return [{
      providerId: state.providerId,
      executionHostId: host.id,
      name: `${provider?.shortName || provider?.name || state.providerId} · ${host.label}`,
      message: state.checkError,
    }]
  }), [executionHosts, providers, versionManager.remoteProviders])
  const providerCheckErrors = useMemo(() => allProviderCheckErrors.filter((result) =>
    dismissedVersions[JSON.stringify([result.executionHostId, result.providerId])] !== updateCheckErrorIdentity(result.message)
  ), [allProviderCheckErrors, dismissedVersions])

  return {
    updates,
    hasCatalog: catalog !== null,
    checking,
    error,
    lastCheckedAt,
    checkNow,
    dismiss,
    dismissAll,
    applyCliProviderVersion,
    installCliProviderVersion,
    applyRemoteHelperUpdate,
    remoteHelperUpdatingId,
    providerStates,
    providerTaskRunning: providerStates.some((provider) => provider.running),
    providerUpdateResults,
    providerCheckErrors,
    hasProviderCheckErrors: allProviderCheckErrors.length > 0,
    dismissProviderCheckError,
    refreshCliProviderVersion,
  }
}

export type UpdateMonitor = ReturnType<typeof useUpdateMonitor>
