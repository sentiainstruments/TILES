/** Versioned, portable, local-only TILES project format. */
export const PROJECT_FORMAT = 'sentia.tiles-project' as const
export const PROJECT_VERSION = 1 as const

export type CustomScale = {
  slot: number
  name: string
  intervals: number[]
}

export type TilesPreset = {
  id: string
  name: string
  description: string
  settings: Record<string, string>
  scales: CustomScale[]
  /** Preview only: the USB protocol cannot activate a scale from the app. */
  previewScaleSlot: number | null
}

export type TilesProject = {
  format: typeof PROJECT_FORMAT
  version: typeof PROJECT_VERSION
  id: string
  name: string
  createdAt: string
  updatedAt: string
  selectedPresetId: string
  presets: TilesPreset[]
}

const record = (v: unknown): v is Record<string, unknown> =>
  typeof v === 'object' && v !== null && !Array.isArray(v)
const text = (v: unknown, max: number) =>
  typeof v === 'string' && v.length > 0 && v.length <= max
const limitedText = (v: unknown, max: number) =>
  typeof v === 'string' && v.length <= max

export function isValidScale(value: unknown): value is CustomScale {
  if (!record(value) || typeof value.slot !== 'number' || !Number.isInteger(value.slot) ||
      Number(value.slot) < 1 || Number(value.slot) > 9 ||
      !text(value.name, 16) || !/^[!-~]+$/.test(value.name as string) ||
      !Array.isArray(value.intervals) || value.intervals.length < 1 ||
      value.intervals.length > 12) return false
  const intervals = value.intervals as number[]
  return intervals.every((n, i) =>
    Number.isInteger(n) && n >= 0 && n < 12 &&
    (i === 0 ? n === 0 : n > intervals[i - 1]))
}

export function validateProject(value: unknown): TilesProject {
  if (!record(value) || value.format !== PROJECT_FORMAT ||
      value.version !== PROJECT_VERSION || !text(value.id, 100) ||
      !text(value.name, 100) || !text(value.createdAt, 40) ||
      !text(value.updatedAt, 40) || !text(value.selectedPresetId, 100) ||
      !Array.isArray(value.presets) || value.presets.length < 1 ||
      value.presets.length > 128) {
    throw new Error('Invalid or unsupported .tilesproject file (requires format version 1).')
  }
  const ids = new Set<string>()
  for (const preset of value.presets) {
    if (!record(preset) || !text(preset.id, 100) || !text(preset.name, 100) ||
        !limitedText(preset.description, 1000) || !record(preset.settings) ||
        !Array.isArray(preset.scales) || preset.scales.length > 9 ||
        !(preset.previewScaleSlot === null ||
          (typeof preset.previewScaleSlot === 'number' && Number.isInteger(preset.previewScaleSlot) &&
           Number(preset.previewScaleSlot) >= 1 && Number(preset.previewScaleSlot) <= 9))) {
      throw new Error('Project contains an invalid preset.')
    }
    if (ids.has(preset.id as string)) throw new Error('Duplicate preset IDs.')
    ids.add(preset.id as string)
    const settings = Object.entries(preset.settings)
    if (settings.length > 256 || !settings.every(([key, val]) =>
      /^[a-z][a-z0-9_.]{0,99}$/.test(key) &&
      typeof val === 'string' && val.length <= 100 && !/[\r\n]/.test(val))) {
      throw new Error('Preset has invalid settings.')
    }
    if (!preset.scales.every(isValidScale) ||
        new Set(preset.scales.map((s: CustomScale) => s.slot)).size !== preset.scales.length) {
      throw new Error('Preset contains an invalid custom scale.')
    }
  }
  if (!ids.has(value.selectedPresetId as string)) {
    throw new Error('Project-selected preset does not exist.')
  }
  return value as unknown as TilesProject
}

export function createId() {
  // Used for local document identities only, not cryptographic authentication.
  return typeof crypto !== 'undefined' && 'randomUUID' in crypto
    ? crypto.randomUUID()
    : `${Date.now()}-${Math.random().toString(36).slice(2)}`
}

export function createPreset(name: string, settings: Record<string, string> = {}): TilesPreset {
  return { id: createId(), name, description: '', settings: { ...settings },
    scales: [], previewScaleSlot: null }
}

export function createProject(name: string, settings: Record<string, string> = {}): TilesProject {
  const now = new Date().toISOString()
  const preset = createPreset('Default', settings)
  return { format: PROJECT_FORMAT, version: PROJECT_VERSION, id: createId(),
    name, createdAt: now, updatedAt: now, selectedPresetId: preset.id,
    presets: [preset] }
}

export function serializeProject(project: TilesProject) {
  validateProject(project)
  return JSON.stringify(project, null, 2) + '\n'
}
