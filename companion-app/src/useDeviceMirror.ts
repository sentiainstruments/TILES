import { useEffect, useState } from 'react'

const MODES = ['melodic', 'duet', 'bass', 'sequencer', 'drums', 'song', 'prism',
  'ableton', 'volca', 'bounce', 'cradle', 'sand', 'ripples', 'orbits'] as const
const VIEWS = ['play', 'modes', 'scales', 'patterns', 'other'] as const
export type MirrorMode = typeof MODES[number]
export type MirrorView = typeof VIEWS[number]
export type MirrorRole = 'root' | 'fifth' | 'third' | 'note' | 'accidental'
const ROLES: Record<string, MirrorRole> = { r: 'root', f: 'fifth', t: 'third', n: 'note', a: 'accidental' }
const NAMES = ['C', 'C♯', 'D', 'D♯', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B']
export type MirrorSnapshot = {
  mode: MirrorMode
  view: MirrorView
  octave: number
  key: number
  scale: number
  pads: { midi: number | null; noteLabel: string; role: MirrorRole; isActiveNote: boolean; cents: number }[]
}
export type MirrorStatus = 'off' | 'checking' | 'hall' | 'mirror' | 'simulator' | 'unavailable'
type MirrorState = {
  status: MirrorStatus
  activePads: ReadonlySet<number>
  pressedPads: ReadonlySet<number>
  snapshot: MirrorSnapshot | null
  error: string
  signature: string
}
const empty = (status: MirrorStatus, error = ''): MirrorState => ({
  status, activePads: new Set(), pressedPads: new Set(), snapshot: null, error, signature: '',
})

export function parseHallFrame(rows: string[]): { activePads: Set<number>; pressedPads: Set<number> } {
  const seen = new Set<number>()
  const touched = new Set<number>()
  const pressed = new Set<number>()
  for (const row of rows) {
    const fields = row.trim().split(',')
    const id = Number(fields[0])
    if (fields.length < 7 || !Number.isInteger(id) || id < 1 || id > 24 ||
        !['0', '1'].includes(fields[6]) || seen.has(id)) {
      throw new Error(`Invalid HALL response: ${row}`)
    }
    seen.add(id)
    const depth = Number(fields[4])
    if (!Number.isFinite(depth) || fields[4].trim() === '') throw new Error(`Invalid HALL depth: ${row}`)
    if (fields[6] === '1') {
      touched.add(id)
      // Firmware's mode menu selects on a press past half travel.
      if (depth > 450) pressed.add(id)
    }
  }
  if (seen.size !== 24) throw new Error(`Incomplete HALL response (${seen.size}/24 pads)`)
  return { activePads: touched, pressedPads: pressed }
}

export function parseHall(rows: string[]): Set<number> {
  return parseHallFrame(rows).activePads
}

export function parseMirror(line: string): { activePads: Set<number>; snapshot: MirrorSnapshot } {
  const [tag, mode, view, oct, key, scale, mask, notes, cents, roles, ...extra] = line.trim().split(',')
  if (tag !== 'MIRROR' || !MODES.includes(mode as MirrorMode) || !VIEWS.includes(view as MirrorView) ||
      !/^-?\d+$/.test(oct ?? '') || !/^\d+$/.test(key ?? '') || !/^\d+$/.test(scale ?? '') ||
      !/^[0-9a-f]{6}$/i.test(mask ?? '') || !/^[0-9a-f]{48}$/i.test(notes ?? '') ||
      !/^[0-9a-f]{6}$/i.test(cents ?? '') || !/^[rftna]{24}$/.test(roles ?? '') || extra.length) {
    throw new Error(`Invalid MIRROR response: ${line}`)
  }
  const activePads = new Set<number>()
  const touchMask = parseInt(mask, 16)
  const centsMask = parseInt(cents, 16)
  const pads = Array.from({ length: 24 }, (_, i) => {
    if (touchMask & (1 << i)) activePads.add(i + 1)
    const raw = parseInt(notes.slice(i * 2, i * 2 + 2), 16)
    if (raw > 127 && raw !== 255) throw new Error('Invalid MIRROR MIDI note')
    const midi = raw === 255 ? null : raw
    const offset = centsMask & (1 << i) ? 50 : 0
    return { midi, cents: offset, role: ROLES[roles[i]], isActiveNote: midi !== null,
      noteLabel: midi === null ? '—' : NAMES[midi % 12] + (Math.floor(midi / 12) - 1) + (offset ? ` +${offset}¢` : '') }
  })
  return { activePads, snapshot: { mode: mode as MirrorMode, view: view as MirrorView,
    octave: Number(oct), key: Number(key), scale: Number(scale), pads } }
}

/** Read only. One outstanding request at a time; old firmware falls back to HALL. */
export function useDeviceMirror(enabled: boolean): MirrorState {
  const [state, setState] = useState<MirrorState>(() => empty(enabled ? 'checking' : 'off'))
  useEffect(() => {
    if (!enabled) { setState(empty('off')); return }
    let cancelled = false
    let timer: number | undefined
    let protocol: 'hall' | 'mirror' | null = null
    const publish = (next: MirrorState) => {
      if (!cancelled) setState(prev => prev.status === next.status && prev.signature === next.signature && prev.error === next.error ? prev : next)
    }
    const schedule = (ms: number) => { if (!cancelled) timer = window.setTimeout(() => { void poll() }, ms) }
    const hall = async () => {
      const frame = parseHallFrame(await window.tiles.usb.command('HALL', true))
      const ids = (pads: Set<number>) => [...pads].sort((a,b) => a-b).join(',')
      publish({ ...frame, status: 'hall', snapshot: null, error: '',
        signature: `${ids(frame.activePads)}|${ids(frame.pressedPads)}` })
    }
    async function poll() {
      if (cancelled) return
      try {
        if (protocol === null) {
          const info = await window.tiles.usb.command('INFO', true)
          if (cancelled) return
          if (info.includes('simulator=1')) { publish(empty('simulator')); schedule(4000); return }
          if (!info.some(line => line.startsWith('firmware='))) throw new Error('No valid TILES INFO response')
          const [line] = await window.tiles.usb.command('MIRROR')
          if (cancelled) return
          if (line?.startsWith('ERR ')) { protocol = 'hall' }
          else {
            const data = parseMirror(line ?? '')
            protocol = 'mirror'
            publish({ ...data, pressedPads: new Set(), status: 'mirror', error: '', signature: line })
            schedule(50)
            return
          }
        }
        if (protocol === 'hall') {
          await hall()
          schedule(100)
        } else {
          const [line] = await window.tiles.usb.command('MIRROR')
          const data = parseMirror(line ?? '')
          publish({ ...data, pressedPads: new Set(), status: 'mirror', error: '', signature: line })
          schedule(50)
        }
      } catch (error) {
        protocol = null
        publish(empty('unavailable', error instanceof Error ? error.message : String(error)))
        schedule(2000)
      }
    }
    publish(empty('checking'))
    void poll()
    return () => { cancelled = true; window.clearTimeout(timer) }
  }, [enabled])
  return enabled ? state : empty('off')
}
