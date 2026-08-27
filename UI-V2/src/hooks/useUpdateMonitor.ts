import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { useAppStore } from '../store/useAppStore'
import { createRequestId, isCefContext, sendToCEF } from '../ipc/cefBridge'
import {
  UPDATE_CHECK_INTERVAL_MS,
  availableUpdates,
  fetchLatestUpdateCatalog,
  readCachedUpdateCatalog,
  type LatestUpdateCatalog,
} from '../services/updateCatalog'

export function useUpdateMonitor() {
  const appVersion = useAppStore((state) => state.appVersion)
  const providers = useAppStore((state) => state.providers)
  const versionManager = useAppStore((state) => state.cliVersionManager)
  const enabled = useAppStore((state) => state.updateChecksEnabled)
  const lastCheckedAt = useAppStore((state) => state.updateLastCheckedAt)
  const dismissedVersions = useAppStore((state) => state.dismissedUpdateVersions)
  const cefStateHydrated = useAppStore((state) => state.lastAppliedStateRevision >= 0)
  const setUpdateSettings = useAppStore((state) => state.setUpdateSettings)
  const refreshCliProviderVersion = useAppStore((state) => state.refreshCliProviderVersion)
  const applyCliProviderVersion = useAppStore((state) => state.applyCliProviderVersion)
  const [catalog, setCatalog] = useState<LatestUpdateCatalog | null>(() => readCachedUpdateCatalog())
  const [checking, setChecking] = useState(false)
  const [error, setError] = useState('')
  const autoCheckAttemptedRef = useRef(false)
  const checkingRef = useRef(false)

  const checkNow = useCallback(async () => {
    if (checkingRef.current) return
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
        await Promise.all(versionManager.providers.map((provider) => refreshCliProviderVersion(provider.providerId)))
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
  }, [refreshCliProviderVersion, setUpdateSettings, versionManager.providers])

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

  const updates = useMemo(() => availableUpdates(
    catalog,
    appVersion,
    versionManager,
    providers,
    dismissedVersions,
  ), [appVersion, catalog, dismissedVersions, providers, versionManager])

  const dismiss = useCallback((id: string, version: string) => {
    void setUpdateSettings({ dismissedUpdateVersions: { ...dismissedVersions, [id]: version } })
  }, [dismissedVersions, setUpdateSettings])

  const dismissAll = useCallback(() => {
    void setUpdateSettings({
      dismissedUpdateVersions: {
        ...dismissedVersions,
        ...Object.fromEntries(updates.map((update) => [update.id, update.latestVersion])),
      },
    })
  }, [dismissedVersions, setUpdateSettings, updates])

  const providerUpdateResults = useMemo(() => versionManager.providers.flatMap((state) => {
    if (state.lastInstallStatus !== 'succeeded' && state.lastInstallStatus !== 'failed') return []
    const provider = providers.find((candidate) => candidate.id === state.providerId)
    return [{
      providerId: state.providerId,
      name: provider?.shortName || provider?.name || state.providerId,
      status: state.lastInstallStatus,
      message: state.message,
      output: state.lastOutput,
      installedVersion: state.installedVersion,
    }]
  }), [providers, versionManager.providers])

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
    providerStates: versionManager.providers,
    providerTaskRunning: versionManager.providers.some((provider) => provider.running),
    providerUpdateResults,
  }
}

export type UpdateMonitor = ReturnType<typeof useUpdateMonitor>
