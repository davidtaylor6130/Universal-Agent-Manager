export interface Provider {
  id: string
  name: string
  shortName: string
  color: string
  description: string
  outputMode?: string
  supportsCli?: boolean
  supportsStructured?: boolean
  structuredProtocol?: string
  structuredPermissionControl?: 'uam' | 'provider'
  terminalPermissionControl?: 'provider'
  npmPackageName?: string
	nativeGoalCommand?: string
  /** Steer reaches the running turn in place; otherwise UAM stops the turn and sends the steer first. */
  supportsNativeSteering?: boolean
}
