import { useCallback, useEffect, useRef, useState } from 'react'
import { ScaleEditorForm } from './ScaleEditor'
import { parseScaleList } from './scalePicker'
import type { CustomScale } from './scalePicker'
import './ScaleEditor.css'

export function CustomScales() {
  const [scales,setScales] = useState<CustomScale[]>([])
  const [slot,setSlot] = useState(1)
  const [loading,setLoading] = useState(true)
  const [busy,setBusy] = useState(false)
  const [ready,setReady] = useState(false)
  const [error,setError] = useState('')
  const [message,setMessage] = useState('')
  const request = useRef(0)
  const writing = useRef(false)
  const refresh = useCallback(async () => {
    const id = ++request.current
    setLoading(true);setError('')
    try {
      const records = parseScaleList(await window.tiles.usb.command('SCALES',true))
      if (id !== request.current) return
      setScales(records);setReady(true)
    } catch (e) {
      if (id !== request.current) return
      setReady(false);setError(`Could not read custom scales: ${String(e)}`)
    } finally { if (id === request.current) setLoading(false) }
  },[])
  useEffect(()=>{
    void refresh()
    const changed = () => { if (!writing.current) void refresh() }
    window.addEventListener('tiles:scales-changed',changed)
    return ()=>{++request.current;window.removeEventListener('tiles:scales-changed',changed)}
  },[refresh])
  const current = scales.find(s=>s.slot===slot)
  const formKey = JSON.stringify([slot,current])
  return <section className="snt-custom-scale-manager" aria-label="Manage custom scales">
    <div className="snt-scale-manager-toolbar">
      <label>Slot <select value={slot} disabled={busy || loading} onChange={e=>{setSlot(Number(e.currentTarget.value));setMessage('')}}>
        {Array.from({length:9},(_,i)=>i+1).map(n=>{
          const record = scales.find(s=>s.slot===n)
          return <option key={n} value={n}>{n} — {record?.name.replace(/_/g,' ') ?? 'Empty'}</option>
        })}
      </select></label>
      <button type="button" disabled={busy || loading} onClick={()=>{setMessage('');void refresh()}}>Refresh</button>
    </div>
    {loading && <p className="snt-scale-hint" role="status">Reading scales…</p>}
    {error && <p className="snt-scale-error" role="alert">{error}</p>}
    {ready && <fieldset className="snt-scale-manager-fieldset" disabled={loading}>
      <ScaleEditorForm key={formKey} slot={slot} initial={current}
        onBusyChange={value=>{writing.current=value;setBusy(value)}}
        onSaved={records=>{
          ++request.current;setLoading(false);setScales(records)
          setMessage(records.some(s=>s.slot===slot)?`Saved scale in slot ${slot}.`:`Cleared slot ${slot}.`)
          window.dispatchEvent(new Event('tiles:scales-changed'))
        }}/>
    </fieldset>}
    {message && <p className="snt-scale-manager-status" role="status">{message}</p>}
  </section>
}
