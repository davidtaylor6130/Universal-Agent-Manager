export type ResourceReferenceType =
  | 'workspace-folder'
  | 'chat'
  | 'file'
  | 'website'
  | 'desktop-app'

export interface ResourceReference {
  id: string
  type: ResourceReferenceType
  target: string
  label: string
}

export interface ResourceCollection {
  id: string
  name: string
  collapsed: boolean
  references: ResourceReference[]
}
