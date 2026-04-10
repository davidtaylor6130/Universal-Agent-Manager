export interface Provider {
  id: string
  name: string
  shortName: string
  color: string
  description: string
}

export interface ProviderFeature {
  id: string
  label: string
  enabled: boolean
  icon: string
}
