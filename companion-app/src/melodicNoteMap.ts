/**
 * UI-only melodic note preview derived from firmware/src/services/note_map.c.
 * Uses preview scale, C key, and local octave; it does NOT read the active
 * key/scale/octave from hardware (those aren't exposed by settings protocol).
 */
export type MelodicRole = 'root' | 'fifth' | 'third' | 'note' | 'accidental'
export type MelodicPadInfo = {
  midi: number | null
  cents: number
  noteLabel: string
  role: MelodicRole
  isActiveNote: boolean
}

const BASE_NOTE = 48  // firmware: pad 19 = C3 in chromatic Rows
const KEYS_WHITE = [0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17, 19]
const NOTES = ['C', 'C♯', 'D', 'D♯', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B']
const NATURALS = new Set([0, 2, 4, 5, 7, 9, 11])
const DEFAULT_SCALE = Array.from({length: 12}, (_, i) => i)

export function getMelodicPadInfo(
  pad: number,
  layout: string,
  scaleIntervals?: number[],
  octave = 0,
): MelodicPadInfo {
  const table = scaleIntervals?.length ? scaleIntervals : DEFAULT_SCALE
  const count = table.length
  const col = (pad - 1) % 6
  const rowUp = 3 - Math.floor((pad - 1) / 6)
  let cents = 0
  let pitchClass: number
  let midi: number | null = null
  let isAbsolute = false

  if (layout === 'keys' || layout === 'tonnetz' || layout === 'quarter_tones') {
    isAbsolute = true
    let semitones: number
    if (layout === 'keys') {
      const white = KEYS_WHITE[Math.floor(rowUp / 2) * 6 + col]
      if (rowUp % 2 === 0) semitones = white
      else {
        const pc = white % 12
        semitones = [0, 2, 5, 7, 9].includes(pc) ? white + 1 : -1
      }
    } else if (layout === 'tonnetz') {
      semitones = col * 7 + rowUp * 4
      while (semitones > 30) semitones -= 12
    } else {
      const step = rowUp * 6 + col
      semitones = Math.floor(step / 2)
      cents = (step % 2) * 50
    }
    if (semitones >= 0) midi = BASE_NOTE + semitones + octave * 12
    pitchClass = semitones < 0 ? -1 : semitones % 12
  } else {
    let degree: number
    if (layout === 'columns') {
      const column = Math.floor(col / 2)
      const slot = rowUp * 2 + (col % 2)
      degree = column * Math.min(count, 8) + slot
    } else if (layout === 'fourths' || layout === 'fifths') {
      const semitones = layout === 'fourths' ? 5 : 7
      // Same rounding as firmware's interval_steps.
      const steps = Math.floor((count * semitones + 6) / 12)
      degree = col + rowUp * steps
    } else {
      degree = rowUp * 6 + col
    }
    const interval = table[degree % count]
    midi = BASE_NOTE + Math.floor(degree / count) * 12 + interval +
      (octave - (layout === 'fifths' ? 1 : 0)) * 12
    pitchClass = interval
  }

  if (midi === null) {
    return { midi: null, cents: 0, noteLabel: '—', role: 'accidental', isActiveNote: false }
  }

  midi = Math.max(0, Math.min(127, midi))
  const roleClass = isAbsolute && (cents !== 0 || !table.includes(pitchClass))
    ? -1 : pitchClass
  const third = table.includes(4) ? 4 : table.includes(3) ? 3 : -1
  const role: MelodicRole = roleClass === 0 ? 'root'
    : roleClass === 7 && table.includes(7) ? 'fifth'
    : roleClass === third && third !== -1 ? 'third'
    : roleClass === -1 || !NATURALS.has(midi % 12) ? 'accidental'
    : 'note'
  const note = NOTES[midi % 12] + (Math.floor(midi / 12) - 1) +
    (cents ? ` +${cents}¢` : '')
  return { midi, cents, noteLabel: note, role, isActiveNote: true }
}
