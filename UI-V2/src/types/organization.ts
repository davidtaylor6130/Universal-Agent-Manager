import type { ResourceReference, ResourceReferenceType } from './resourceCollection'
export interface ResourceMembership { collectionId: string; index: number; reference: Omit<ResourceReference, 'customIcon'> & { customIcon: ResourceReference['customIcon'] | null } }
export type OrganizationChange =
  | { kind: 'pin'; chatId: string; expected: boolean; replacement: boolean }
  | { kind: 'folderOrder'; expected: string[]; replacement: string[] }
  | { kind: 'referenceOrder'; collectionId: string; expected: string[]; replacement: string[] }
  | { kind: 'collectionOrder'; expected: string[]; replacement: string[] }
  | { kind: 'memberships'; type: ResourceReferenceType; target: string; expected: ResourceMembership[]; replacement: ResourceMembership[] }
export interface OrganizationUndo { label: string; change: OrganizationChange }
