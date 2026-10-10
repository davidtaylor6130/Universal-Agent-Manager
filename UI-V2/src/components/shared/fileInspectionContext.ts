import { createContext } from 'react'
import type { FileReference } from '../../utils/fileReferences'

export const FileInspectionContext = createContext<((reference: FileReference) => void) | null>(null)
