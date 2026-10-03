import type { CustomIcon } from './customIcon'
export type ResourceReferenceType =
  | 'workspace-folder'
  | 'chat'
  | 'file'
  | 'website'
  | 'desktop-app'

export interface ResourceReference {
  customIcon?: CustomIcon
  id: string
  type: ResourceReferenceType
  target: string
  label: string
}

export interface ResourceCollection {
  customIcon?: CustomIcon
  id: string
  name: string
  collapsed: boolean
  references: ResourceReference[]
}
