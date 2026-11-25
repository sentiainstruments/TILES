export type CustomScale = { slot: number; name: string; intervals: number[]; metadata?: string }
export type PickerScale = { id: string; name: string; intervals: number[]; customSlot?: number }

// Physical pad order, firmware/board-bringup: services/note_map.c SCALE_GRID_ORDER.
const builtins: PickerScale[] = [
  { id: 'chromatic', name: 'Chromatic', intervals: [0,1,2,3,4,5,6,7,8,9,10,11] },
  { id: 'major', name: 'Major', intervals: [0,2,4,5,7,9,11] },
  { id: 'minor', name: 'Minor', intervals: [0,2,3,5,7,8,10] },
  { id: 'dorian', name: 'Dorian', intervals: [0,2,3,5,7,9,10] },
  { id: 'lydian', name: 'Lydian', intervals: [0,2,4,6,7,9,11] },
  { id: 'mixolydian', name: 'Mixolydian', intervals: [0,2,4,5,7,9,10] },
  { id: 'major-blues', name: 'Major Blues', intervals: [0,2,3,4,7,9] },
  { id: 'minor-blues', name: 'Minor Blues', intervals: [0,3,5,6,7,10] },
  { id: 'arabian', name: 'Arabian', intervals: [0,1,4,5,7,8,11] },
  { id: 'diminished', name: 'Diminished', intervals: [0,2,3,5,6,8,9,11] },
  { id: 'major-pentatonic', name: 'Major Pentatonic', intervals: [0,2,4,7,9] },
  { id: 'minor-pentatonic', name: 'Minor Pentatonic', intervals: [0,3,5,7,10] },
  { id: 'egyptian', name: 'Egyptian', intervals: [0,2,5,7,10] },
  { id: 'whole-tone', name: 'Whole Tone', intervals: [0,2,4,6,8,10] },
  { id: 'japanese', name: 'Japanese', intervals: [0,1,5,7,8] },
]
export type FactoryOverrides = Record<string, { name: string; intervals: number[] }>
export function factoryScale(id: string): PickerScale | undefined {
  const scale = builtins.find(s => s.id === id)
  return scale ? { ...scale, intervals: [...scale.intervals] } : undefined
}
export function makeScaleLibrary(scales: CustomScale[], overrides: FactoryOverrides = {}): PickerScale[] {
  return [...builtins.map(scale => {
    const edit = overrides[scale.id]
    return edit && validIntervals(edit.intervals) ? { ...scale, name: edit.name, intervals: [...edit.intervals] } : scale
  }), ...Array.from({length: 9}, (_, i) => {
    const slot = i+1
    const custom = scales.find(s => s.slot === slot)
    return { id: String(slot), name: custom?.name.replace(/_/g, ' ') ?? `Custom ${slot}`, intervals: custom?.intervals ?? [], customSlot: slot }
  })]
}
export function validIntervals(v: number[]) {
  return v.length >= 1 && v.length <= 12 && v[0] === 0 && v.every((n,i) => Number.isInteger(n) && n >= 0 && n <= 11 && (i === 0 || n > v[i-1]))
}
export function parseScaleList(rows: string[]): CustomScale[] {
  const error = rows.find(row => row.startsWith('ERR'))
  if (error) throw new Error(error)
  const scales: CustomScale[] = []
  for (const row of rows) {
    if (!row.trim() || row.trim() === 'OK') continue
    const m = row.trim().match(/^([1-9]) (\S+) (\d+(?:,\d+)*)(?:\s+(.*))?$/)
    if (!m) throw new Error(`Unexpected scale record: ${row}`)
    const intervals = m[3].split(',').map(Number)
    if (!validIntervals(intervals) || scales.some(s => s.slot === Number(m[1]))) throw new Error(`Invalid scale record: ${row}`)
    scales.push({slot:Number(m[1]),name:m[2],intervals, ...(m[4] ? {metadata:m[4]} : {})})
  }
  return scales
}
export function scalePutCommand(slot: number, name: string, intervals: number[]) {
  const normalized = name.trim().replace(/\s+/g,'_')
  if (!Number.isInteger(slot) || slot < 1 || slot > 9) throw new Error('Invalid custom scale slot.')
  if (!/^[!-~]{1,16}$/.test(normalized)) throw new Error('Use a name of 1–16 ASCII characters.')
  if (!validIntervals(intervals)) throw new Error('Invalid scale intervals.')
  return `SCALE PUT ${slot} ${normalized} ${intervals.join(',')}`
}
export type ScaleCommand = (command: string, multiLine?: boolean) => Promise<string[]>
export async function createScale(command: ScaleCommand, slot: number, name: string, intervals: number[]) {
  const put = scalePutCommand(slot, name, intervals)
  const before = parseScaleList(await command('SCALES', true))
  if (before.some(s => s.slot === slot)) throw new Error('This slot is already occupied. Close and reopen the scale menu.')
  const response = await command(put)
  if (response[0] !== 'OK') throw new Error(response[0] || 'No response to SCALE PUT')
  try {
    const after = parseScaleList(await command('SCALES', true))
    const saved = after.find(s => s.slot === slot)
    if (!saved || saved.name !== name.trim().replace(/\s+/g,'_') || saved.intervals.join(',') !== intervals.join(',')) throw new Error('Scale readback did not match.')
    return after
  } catch (e) {
    throw new Error(`Device acknowledged the save, but readback failed. Refresh the scale menu before retrying. ${String(e)}`)
  }
}

export function sameScale(a: CustomScale | undefined, b: CustomScale | undefined) {
  return a === undefined || b === undefined ? a === b :
    a.slot === b.slot && a.name === b.name && a.intervals.join(',') === b.intervals.join(',') &&
    (a.metadata ?? '') === (b.metadata ?? '')
}

async function checkSlot(command: ScaleCommand, expected: CustomScale) {
  const before = parseScaleList(await command('SCALES', true))
  if (!sameScale(before.find(s => s.slot === expected.slot), expected)) {
    throw new Error('This slot changed on TILES. Refresh before editing it.')
  }
}

export async function updateScale(command: ScaleCommand, expected: CustomScale, name: string, intervals: number[]) {
  const put = scalePutCommand(expected.slot, name, intervals)
  await checkSlot(command, expected)
  // Edits become user-authored scales, not the original content pack record.
  const response = await command(put)
  if (response[0] !== 'OK') throw new Error(response[0] || 'No response to SCALE PUT')
  try {
    const after = parseScaleList(await command('SCALES', true))
    const saved = after.find(s => s.slot === expected.slot)
    if (!saved || saved.name !== name.trim().replace(/\s+/g, '_') || saved.intervals.join(',') !== intervals.join(',')) throw new Error('Scale readback did not match.')
    return after
  } catch (e) { throw new Error(`Device acknowledged the save, but readback failed. Refresh before retrying. ${String(e)}`) }
}

export async function deleteScale(command: ScaleCommand, expected: CustomScale) {
  if (!Number.isInteger(expected.slot) || expected.slot < 1 || expected.slot > 9) throw new Error('Invalid scale slot.')
  await checkSlot(command, expected)
  const response = await command(`SCALE DELETE ${expected.slot}`)
  if (response[0] !== 'OK') throw new Error(response[0] || 'No response to SCALE DELETE')
  try {
    const after = parseScaleList(await command('SCALES', true))
    if (after.some(s => s.slot === expected.slot)) throw new Error('Slot is still occupied.')
    return after
  } catch (e) { throw new Error(`Device acknowledged the deletion, but readback failed. Refresh before retrying. ${String(e)}`) }
}
