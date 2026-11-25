// UI navigation only: no device commands or preset writes.
export type ModeProjectTarget = {
  slot: number
  modeId: string | null
  name: string
  kind: 'existing' | 'new'
}
export const OPEN_MODE_EDITOR = 'tiles:open-mode-editor'
export function openModeEditor(key: string) {
  window.dispatchEvent(new CustomEvent(OPEN_MODE_EDITOR, { detail: key }))
}
