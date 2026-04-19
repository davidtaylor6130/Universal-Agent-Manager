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
}
