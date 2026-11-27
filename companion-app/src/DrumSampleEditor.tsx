import { useEffect, useRef, useState } from 'react'
import { WorkspaceIcon } from './WorkspaceIcon'
import { asWav, parseDrumSlots, prepareDrumSample, SAMPLE_RATE } from './drumSample'
import type { DrumSlot, PreparedSample } from './drumSample'
import './DrumSampleEditor.css'

type SampleUpload = { slot: number; name: string; channels: 1 | 2; frames: number; bytes: number[] }
// Optional IPC extension; UI-only installations can still list, play and preview.
const sampleUsb = () => window.tiles.usb as typeof window.tiles.usb & { uploadDrumSample?: (sample: SampleUpload) => Promise<void> }
const message = (e: unknown) => e instanceof Error ? e.message : String(e)
export function DrumSampleEditor({ onSlotsChange }: { onSlotsChange?: (slots: DrumSlot[]) => void } = {}) {
  const [slots,setSlots] = useState<DrumSlot[]>([])
  const [busy,setBusy] = useState(false)
  const [error,setError] = useState('')
  const [status,setStatus] = useState('')
  const [target,setTarget] = useState<number | null>(null)
  const [file,setFile] = useState<File | null>(null)
  const [mono,setMono] = useState(true)
  const [normalize,setNormalize] = useState(false)
  const [prepared,setPrepared] = useState<PreparedSample | null>(null)
  const [preparing,setPreparing] = useState(false)
  const [name,setName] = useState('')
  const [url,setUrl] = useState('')
  const audio = useRef<HTMLAudioElement>(null)
  const lock = useRef(false)
  const mounted = useRef(false)
  const supported = typeof sampleUsb().uploadDrumSample === 'function'
  async function refresh() {
    const rows = await window.tiles.usb.command('SAMPLE LIST',true)
    const next = parseDrumSlots(rows)
    if (mounted.current) {
      setSlots(next)
      onSlotsChange?.(next)
    }
  }
  async function run(action: () => Promise<void>) {
    if (lock.current) return
    lock.current = true; setBusy(true); setError(''); setStatus('')
    try { await action() } catch(e) { if(mounted.current) setError(message(e)) }
    finally { lock.current=false; if(mounted.current) setBusy(false) }
  }
  useEffect(() => {
    mounted.current = true
    void run(refresh)
    return () => { mounted.current = false }
    // Initial load only; refresh is also available on demand.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])
  useEffect(() => {
    setPrepared(null); setUrl(''); setPreparing(false)
    if (!file) return
    let cancelled=false; let objectUrl=''
    setPreparing(true); setError('')
    void prepareDrumSample(file,mono,normalize).then(sample => {
      if(cancelled) return
      objectUrl=URL.createObjectURL(asWav(sample)); setUrl(objectUrl); setPrepared(sample); setName(sample.name)
    }).catch(e=>{if(!cancelled)setError(message(e))}).finally(()=>{if(!cancelled)setPreparing(false)})
    return ()=>{ cancelled=true; if(objectUrl) URL.revokeObjectURL(objectUrl) }
  },[file,mono,normalize])
  async function command(text: string) {
    const rows = await window.tiles.usb.command(text)
    if (rows.length !== 1 || rows[0] !== 'OK') throw new Error(rows.join(' ') || 'No reply from TILES')
  }
  function choose(slot: number) { setTarget(slot); setFile(null); setPrepared(null); setName(''); setStatus(''); setError('') }
  function upload() {
    const api = sampleUsb().uploadDrumSample
    if (!api || !prepared || target===null || busy || preparing || !/^[A-Za-z0-9_-]{1,23}$/.test(name)) return
    const occupied = slots.find(s=>s.slot===target)?.name
    const warning = occupied ? `Replace slot ${target} (${occupied})? The original cannot be downloaded by this app. An interrupted upload can leave the slot empty.` : `Write ${name} to empty slot ${target}?`
    if (!window.confirm(`${warning}\n\n${prepared.trimmed?'Only the displayed, trimmed portion will be uploaded.\n':''}Stop the sequencer and all audio before continuing.`)) return
    audio.current?.pause()
    void run(async()=>{
      try {
        await api({slot:target,name,channels:prepared.channels,frames:prepared.frames,bytes:Array.from(prepared.bytes)})
      } catch(e) { try { await refresh() } catch { /* connection may need recovery */ } throw e }
      await refresh(); setStatus(`Saved ${name} to slot ${target}.`); setFile(null); setTarget(null)
    })
  }
  return <section className="snt-drum-samples" aria-label="Onboard drum samples">
    <div className="snt-drum-heading">
      <div><h4>Drum samples</h4><span>{slots.length ? `${slots.filter(s=>s.name).length} / 8 slots used` : 'Onboard sample storage'}</span></div>
      <button type="button" title="Refresh sample list" aria-label="Refresh sample list" disabled={busy} onClick={()=>void run(refresh)}><WorkspaceIcon kind="refresh"/></button>
    </div>
    {error && <p className="snt-drum-error" role="alert">{error}</p>}
    {status && <p className="snt-drum-status" role="status">{status}</p>}
    {!slots.length && <p className="snt-muted">{busy?'Reading sample slots…':'No sample list available. Connect a supported TILES and refresh.'}</p>}
    <div className="snt-drum-grid">
      {slots.map(s=><article key={s.slot} className={`snt-drum-slot ${s.name?'occupied':''} ${target===s.slot?'selected':''}`}>
        <div className="snt-drum-slot-top"><span>Slot {s.slot}</span>{s.name && <button type="button" title="Play on TILES" aria-label={`Play ${s.name} on TILES`} disabled={busy} onClick={()=>void run(async()=>{await command(`SAMPLE PLAY ${s.slot} 100`);setStatus(`TILES accepted playback for ${s.name}. Check its audio output.`)})}><WorkspaceIcon kind="speaker"/></button>}</div>
        <strong title={s.name}>{s.name||'Empty'}</strong>
        <small>{s.channels ? `${s.channels===1?'Mono':'Stereo'} · ${(s.frames/SAMPLE_RATE).toFixed(3)} s` : 'Available'}</small>
        <div className="snt-drum-actions">
          <button type="button" disabled={busy} onClick={()=>choose(s.slot)}>{s.name?'Replace':'Load sample'}</button>
          {s.name && <button type="button" disabled={busy} className="snt-drum-delete" onClick={()=>{if(window.confirm(`Delete ${s.name} from slot ${s.slot}? This cannot be undone without the original file.`)) void run(async()=>{await command(`SAMPLE DELETE ${s.slot}`);await refresh();setStatus(`Cleared slot ${s.slot}.`);if(target===s.slot){setTarget(null);setFile(null)}})}}>Delete</button>}
        </div>
      </article>)}
    </div>
    {target!==null && <div className="snt-drum-staging">
      <div className="snt-drum-heading"><h4>Prepare sample · Slot {target}</h4><button type="button" disabled={busy} onClick={()=>{setTarget(null);setFile(null)}}>Cancel</button></div>
      <label className="snt-drum-drop" onDragOver={e=>e.preventDefault()} onDrop={e=>{e.preventDefault();if(!busy && e.dataTransfer.files[0])setFile(e.dataTransfer.files[0])}}>
        <span>{file?file.name:'Drop a WAV file or browse'}</span><input key={target} type="file" accept=".wav,audio/wav,audio/x-wav" disabled={busy} onChange={e=>setFile(e.currentTarget.files?.[0]??null)}/>
      </label>
      <div className="snt-drum-options"><label><input type="checkbox" checked={mono} disabled={busy} onChange={e=>setMono(e.currentTarget.checked)}/> Convert to mono</label><label><input type="checkbox" checked={normalize} disabled={busy} onChange={e=>setNormalize(e.currentTarget.checked)}/> Normalize</label></div>
      {preparing && <p className="snt-muted">Converting audio…</p>}
      {prepared && <>
        <svg className="snt-drum-wave" viewBox="0 0 384 64" preserveAspectRatio="none" role="img" aria-label="Waveform of prepared sample">
          <path d="M0 32 H384" stroke="currentColor" opacity=".35"/>{prepared.waveform.map((v,i)=><line key={i} x1={i*4+2} x2={i*4+2} y1={32-v*30} y2={32+v*30} stroke="currentColor" strokeWidth="2"/>)}
        </svg>
        <small>{(prepared.frames/SAMPLE_RATE).toFixed(3)} s · 48 kHz · 16-bit · {prepared.channels===1?'Mono':'Stereo'}</small>
        {prepared.trimmed && <p className="snt-drum-error">Trimmed to the device limit ({prepared.channels===1?'2 seconds mono':'1 second stereo'}). Preview the result before uploading.</p>}
        <label className="snt-drum-name">Sample name <input value={name} maxLength={23} disabled={busy} onChange={e=>setName(e.currentTarget.value)} /></label>
        <audio ref={audio} src={url} controls preload="metadata" aria-label="Local sample preview"/>
        <button type="button" className="snt-drum-upload" disabled={!supported||busy||preparing||!/^[A-Za-z0-9_-]{1,23}$/.test(name)} onClick={upload}>{busy?'Working…':`Upload to slot ${target}`}</button>
        {!supported && <p className="snt-muted">Upload bridge not installed. Local preview remains available.</p>}
      </>}
    </div>}
  </section>
}
