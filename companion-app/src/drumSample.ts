export type DrumSlot = { slot: number; name: string; channels: 1 | 2 | null; frames: number }
export type PreparedSample = { name: string; channels: 1 | 2; frames: number; bytes: Uint8Array; waveform: number[]; trimmed: boolean }
export const SAMPLE_RATE = 48000
export function parseDrumSlots(rows: string[]): DrumSlot[] {
  if (rows.some(row => row.startsWith('ERR '))) throw new Error(rows.join(' '))
  const slots = new Map<number, DrumSlot>()
  for (const line of rows) {
    const parts = line.trim().split(/\s+/)
    const slot = Number(parts[0]); const frames = Number(parts[3])
    if (!Number.isInteger(slot) || slot < 1 || slot > 8 || slots.has(slot)) throw new Error(`Invalid sample row: ${line}`)
    if (parts.length === 2 && parts[1] === 'empty') slots.set(slot, {slot, name: '', channels: null, frames: 0})
    else if (parts.length === 4 && ['mono','stereo'].includes(parts[2]) && Number.isInteger(frames) && frames > 0 && frames <= (parts[2] === 'mono' ? 96000 : 48000)) {
      slots.set(slot, {slot, name: parts[1], channels: parts[2] === 'mono' ? 1 : 2, frames})
    } else throw new Error(`Invalid sample row: ${line}`)
  }
  if (slots.size !== 8) throw new Error(`Expected 8 sample slots; got ${slots.size}`)
  return [...slots.values()].sort((a,b) => a.slot-b.slot)
}
export function sampleName(fileName: string): string {
  return fileName.replace(/\.[^.]+$/, '').replace(/[^a-zA-Z0-9_-]/g, '_').slice(0,23) || 'sample'
}
export async function prepareDrumSample(file: File, mono: boolean, normalize: boolean): Promise<PreparedSample> {
  if (file.size > 32 * 1024 * 1024) throw new Error('Choose an audio file under 32 MB.')
  const decoder = new AudioContext()
  let decoded: AudioBuffer
  try { decoded = await decoder.decodeAudioData(await file.arrayBuffer()) }
  finally { await decoder.close() }
  const channels: 1 | 2 = mono || decoded.numberOfChannels === 1 ? 1 : 2
  const maxFrames = channels === 1 ? 96000 : 48000
  const requested = Math.max(1, Math.ceil(decoded.length * SAMPLE_RATE / decoded.sampleRate))
  const frames = Math.min(maxFrames, requested)
  const renderer = new OfflineAudioContext(channels, frames, SAMPLE_RATE)
  const source = renderer.createBufferSource()
  source.buffer = decoded; source.connect(renderer.destination); source.start()
  const rendered = await renderer.startRendering()
  const samples = Array.from({length: channels}, (_, ch) => rendered.getChannelData(ch))
  let peak = 0
  for (const channel of samples) for (const x of channel) peak = Math.max(peak, Math.abs(x))
  const gain = normalize && peak > 0 ? 0.95 / peak : 1
  const bytes = new Uint8Array(frames * channels * 2)
  const data = new DataView(bytes.buffer)
  const waveform = Array<number>(96).fill(0)
  for (let f = 0; f < frames; f++) for (let ch = 0; ch < channels; ch++) {
    const v = Math.max(-1, Math.min(1, samples[ch][f] * gain))
    data.setInt16((f*channels+ch)*2, Math.round(v < 0 ? v*32768 : v*32767), true)
    const bin = Math.min(95, Math.floor(f * 96 / frames))
    waveform[bin] = Math.max(waveform[bin], Math.abs(v))
  }
  return {name: sampleName(file.name), channels, frames, bytes, waveform, trimmed: requested > maxFrames}
}
export function asWav(sample: PreparedSample): Blob {
  const b = new ArrayBuffer(44 + sample.bytes.length), v = new DataView(b)
  const tag = (at: number, s: string) => { for (let i=0;i<s.length;i++) v.setUint8(at+i,s.charCodeAt(i)) }
  tag(0,'RIFF'); v.setUint32(4,b.byteLength-8,true); tag(8,'WAVE'); tag(12,'fmt ')
  v.setUint32(16,16,true); v.setUint16(20,1,true); v.setUint16(22,sample.channels,true)
  v.setUint32(24,SAMPLE_RATE,true); v.setUint32(28,SAMPLE_RATE*sample.channels*2,true)
  v.setUint16(32,sample.channels*2,true); v.setUint16(34,16,true); tag(36,'data'); v.setUint32(40,sample.bytes.length,true)
  new Uint8Array(b,44).set(sample.bytes)
  return new Blob([b],{type:'audio/wav'})
}
