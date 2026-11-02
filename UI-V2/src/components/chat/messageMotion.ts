import { motionAllowed } from '../ui/Overlay'

/** Phases set as `data-message-motion` on `.uam-message-list`; rows after the `data-motion-anchor` row are affected. */
export type MessageMotionPhase = 'out-next' | 'out-prev' | 'in-next' | 'in-prev' | 'editing' | 'collapse'

/** Branch swipe outranks edit state; reduced motion / Motion Off drops every movement phase (edit dim stays as paint). */
export function messageMotionPhase({ branch, editing, saving }: { branch: MessageMotionPhase | null; editing: boolean; saving: boolean }): MessageMotionPhase | undefined {
  if (branch && motionAllowed()) return branch
  if (!editing) return undefined
  return saving && motionAllowed() ? 'collapse' : 'editing'
}
