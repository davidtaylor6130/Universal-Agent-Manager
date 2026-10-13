import { createContext } from 'react'
import type { FileReference } from '../../utils/fileReferences'

export const FileInspectionContext = createContext<((reference: FileReference) => void) | null>(null)

/** The chat's own branch names (workspace branch and base ref), used to chip branch mentions. */
export const KnownBranchesContext = createContext<readonly string[]>([])
