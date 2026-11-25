import { useId, useRef, useState } from 'react'
import { FloatingPanel } from './controls/FloatingPanel'
import { createScale, updateScale, deleteScale } from './scalePicker'
import type { CustomScale, PickerScale } from './scalePicker'
import './ScaleEditor.css'

const WHITE = [0,2,4,5,7,9,11]
const BLACK = [{n:1,pos:1},{n:3,pos:2},{n:6,pos:4},{n:8,pos:5},{n:10,pos:6}]
const NOTES = ['C','C♯','D','D♯','E','F','F♯','G','G♯','A','A♯','B']
const command = (cmd: string, multi?: boolean) => window.tiles.usb.command(cmd,multi)

function ScaleKeyboard({notes,busy,toggle}: {notes:number[];busy:boolean;toggle:(note:number)=>void}) {
  return <>
    <div className="snt-scale-keyboard" role="group" aria-label="Scale notes relative to C root">
      {WHITE.map(n=><button key={n} type="button" className={`snt-scale-white ${notes.includes(n)?'active':''}`} aria-pressed={notes.includes(n)} aria-label={n===0?'C root (required)':NOTES[n]} disabled={busy || n===0} onClick={()=>toggle(n)}></button>)}
      {BLACK.map(({n,pos})=><button key={n} type="button" className={`snt-scale-black ${notes.includes(n)?'active':''}`} style={{left:`${pos/7*100}%`}} aria-pressed={notes.includes(n)} aria-label={NOTES[n]} disabled={busy} onClick={()=>toggle(n)}></button>)}
    </div>
    <div className="snt-scale-readout"><span>{notes.map(n=>NOTES[n]).join(' · ')}</span><small>{notes.length} / 12</small></div>
  </>
}

type FormProps = {
  slot: number
  initial?: CustomScale
  onSaved: (scales: CustomScale[]) => void
  onBusyChange?: (busy: boolean) => void
  onUse?: () => void
  showReset?: boolean
}

// Both the floating creation window and inline slot manager use this form.
// Mount with a slot/record key to reset drafts when the selected record changes.
export function ScaleEditorForm({slot,initial,onSaved,onBusyChange,onUse,showReset}: FormProps) {
  const inputId = useId()
  const [name,setName] = useState(initial?.name.replace(/_/g,' ') ?? '')
  const [notes,setNotes] = useState<number[]>(initial ? [...initial.intervals] : [0])
  const [busy,setBusy] = useState(false)
  const [error,setError] = useState('')
  const [confirmDelete,setConfirmDelete] = useState(false)
  const lock = useRef(false)
  const normalizedName = name.trim().replace(/\s+/g,'_')
  const changed = !initial || normalizedName !== initial.name || notes.join(',') !== initial.intervals.join(',')
  const validName = /^[!-~]{1,16}$/.test(normalizedName)
  function toggle(n:number) {
    if (n === 0 || lock.current) return
    setNotes(old => old.includes(n) ? old.filter(v => v !== n) : [...old,n].sort((a,b)=>a-b))
    setConfirmDelete(false)
  }
  async function perform(remove = false) {
    if (lock.current || (remove ? !initial || !confirmDelete : !validName || !changed)) return
    lock.current = true
    setBusy(true); onBusyChange?.(true); setError('')
    try {
      const scales = remove && initial ? await deleteScale(command,initial)
        : initial ? await updateScale(command,initial,name,notes)
        : await createScale(command,slot,name,notes)
      onSaved(scales)
    } catch (e) { setError(e instanceof Error ? e.message : String(e)) }
    finally { lock.current = false; setBusy(false); onBusyChange?.(false); setConfirmDelete(false) }
  }
  return <form className="snt-scale-editor" onSubmit={event=>{event.preventDefault();void perform()}}>
    <div className={showReset ? 'snt-scale-factory-name' : undefined}>
      <div className="snt-scale-name-row">
        <label htmlFor={inputId}>Scale name</label>
        <input id={inputId} value={name} maxLength={16} required disabled={busy} placeholder="My scale" onChange={e=>{setName(e.currentTarget.value);setConfirmDelete(false)}}/>
      </div>
      {showReset && <ResetIcon label={initial ? 'Restore last saved scale' : 'Reset new scale'} disabled={busy} onClick={()=>{setName(initial?.name.replace(/_/g,' ') ?? '');setNotes(initial ? [...initial.intervals] : [0]);setError('');setConfirmDelete(false)}}/>}
    </div>
    <ScaleKeyboard notes={notes} busy={busy} toggle={toggle}/>
    <div className="snt-scale-actions">
      <div className="snt-scale-secondary-actions">
        <button type="button" disabled={busy} onClick={()=>{setNotes([0]);setConfirmDelete(false)}}>Clear</button>
        {initial && <button type="button" className="snt-scale-delete" disabled={busy} onClick={()=>setConfirmDelete(v=>!v)}>Delete</button>}
      </div>
      <span/>
      <button type="submit" className="snt-scale-save" disabled={busy || !validName || !changed || confirmDelete}>{busy?'Working…':'Save to TILES'}</button>
    </div>
    {onUse && initial && <button type="button" className="snt-scale-use" disabled={busy || changed || confirmDelete} onClick={onUse}>Use scale</button>}
    {confirmDelete && <div className="snt-scale-confirm" role="group" aria-label="Confirm scale deletion">
      <span>Delete {initial?.name.replace(/_/g,' ')} from slot {slot}?</span>
      <div className="snt-scale-secondary-actions"><button type="button" disabled={busy} onClick={()=>setConfirmDelete(false)}>Cancel</button><button type="button" className="snt-scale-delete" disabled={busy} onClick={()=>void perform(true)}>Confirm delete</button></div>
    </div>}
    {error && <p role="alert" className="snt-scale-error">{error}</p>}
    {initial?.metadata && changed && /(?:^|\s)(?:pack|item)=/.test(initial.metadata) && <small className="snt-scale-hint">Saving an edit replaces the original content-pack metadata.</small>}
  </form>
}

// The existing simulator API remains unchanged: this is still create-only.
export function ScaleEditor({slot,onClose,onSaved}: {slot:number;onClose:()=>void;onSaved:(scales:CustomScale[])=>void}) {
  const busy = useRef(false)
  return <FloatingPanel title={`Custom Scale ${slot}`} onClose={()=>{if(!busy.current)onClose()}}>
    <ScaleEditorForm key={slot} slot={slot} onSaved={onSaved} onBusyChange={value=>{busy.current=value}}/>
  </FloatingPanel>
}

function ResetIcon({label,disabled,onClick}: {label:string;disabled?:boolean;onClick:()=>void}) {
  return <button type="button" className="snt-scale-reset-icon" title={label} aria-label={label} disabled={disabled} onClick={onClick}>
    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true"><path d="M20 7v5h-5M4 17v-5h5"/><path d="M6.1 7a7 7 0 0 1 11.6-1L20 9M4 15l2.3 3A7 7 0 0 0 17.9 17"/></svg>
  </button>
}

type PadEditorProps = {
  scale: PickerScale
  factory?: PickerScale
  initial?: CustomScale
  onClose: () => void
  onSaved: (scales: CustomScale[]) => void
  onFactorySave: (edit: {name:string;intervals:number[]} | null) => void
  onUse: () => void
}

export function ScalePadEditor(props: PadEditorProps) {
  const busy = useRef(false)
  const {scale,factory,initial,onClose,onSaved,onFactorySave,onUse} = props
  return <FloatingPanel title={factory ? `Edit ${factory.name}` : `Custom Scale ${scale.customSlot}`} onClose={()=>{if(!busy.current)onClose()}}>
    {factory ? <FactoryScaleForm scale={scale} factory={factory} onSave={onFactorySave} onUse={onUse}/>
      : <ScaleEditorForm key={JSON.stringify([scale.customSlot,initial])} slot={scale.customSlot!} initial={initial} onSaved={onSaved} onUse={onUse} showReset onBusyChange={value=>{busy.current=value}}/>}
  </FloatingPanel>
}

function FactoryScaleForm({scale,factory,onSave,onUse}: {scale:PickerScale;factory:PickerScale;onSave:PadEditorProps['onFactorySave'];onUse:()=>void}) {
  const id = useId()
  const [name,setName] = useState(scale.name)
  const [notes,setNotes] = useState([...scale.intervals])
  const changed = name.trim() !== scale.name || notes.join(',') !== scale.intervals.join(',')
  const valid = name.trim().length > 0 && name.trim().length <= 16
  function toggle(n:number) { if(n!==0)setNotes(old=>old.includes(n)?old.filter(v=>v!==n):[...old,n].sort((a,b)=>a-b)) }
  function restore() { setName(factory.name);setNotes([...factory.intervals]);onSave(null) }
  return <form className="snt-scale-editor" onSubmit={event=>{event.preventDefault();if(valid && changed)onSave({name:name.trim(),intervals:[...notes]})}}>
    <div className="snt-scale-factory-name"><div className="snt-scale-name-row"><label htmlFor={id}>Scale name</label><input id={id} value={name} maxLength={16} onChange={event=>setName(event.currentTarget.value)}/></div><ResetIcon label="Restore factory scale" onClick={restore}/></div>
    <ScaleKeyboard notes={notes} busy={false} toggle={toggle}/>
    <div className="snt-scale-actions"><button type="button" onClick={()=>setNotes([0])}>Clear</button><span/><button type="submit" className="snt-scale-save" disabled={!valid || !changed}>Save preview</button></div>
    <button type="button" className="snt-scale-use" disabled={changed} onClick={onUse}>Use scale</button>
  </form>
}
