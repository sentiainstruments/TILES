import { useCallback, useEffect, useState } from 'react'
import { CURVE_PRESETS, ResponseCurveEditor } from './controls/ResponseCurveEditor'
import type { ResponseCurve } from './controls/ResponseCurveEditor'
import { SchemaSettings } from './SchemaSettings'

type Setting = {
  key: string
  type: 'bool' | 'enum' | 'int' | 'uint' | 'float' | 'color'
  options?: string[]
  min?: number
  max?: number
  persist: boolean
}

type GlobalProps = { connected: boolean }

function readSchema(lines: string[]): Setting[] {
  const problem = lines.find(line => line.startsWith('ERR '))
  if (problem) throw new Error(problem)
  return lines.flatMap(line => {
    const fields: Record<string, string> = {}
    for (const token of line.trim().split(/\s+/)) {
      const index = token.indexOf('=')
      if (index > 0) fields[token.slice(0, index)] = token.slice(index + 1)
    }
    if (!fields.key || !['bool', 'enum', 'int', 'uint', 'float', 'color'].includes(fields.type)) return []
    return [{
      key: fields.key, type: fields.type as Setting['type'], persist: fields.persist !== '0',
      options: fields.values?.split('|'),
      min: fields.min !== undefined && Number.isFinite(Number(fields.min)) ? Number(fields.min) : undefined,
      max: fields.max !== undefined && Number.isFinite(Number(fields.max)) ? Number(fields.max) : undefined,
    }]
  })
}

function readValues(lines: string[]): Record<string, string> {
  const problem = lines.find(line => line.startsWith('ERR '))
  if (problem) throw new Error(problem)
  const values: Record<string, string> = {}
  for (const line of lines) {
    const separator = line.indexOf('=')
    if (separator > 0) values[line.slice(0, separator)] = line.slice(separator + 1)
  }
  return values
}

function DeviceControl({ setting, label, value, busy, onUpdate }: {
  setting: Setting
  label: string
  value: string | undefined
  busy: boolean
  onUpdate: (setting: Setting, next: string) => Promise<void>
}) {
  const [draft, setDraft] = useState(value ?? '')
  useEffect(() => { setDraft(value ?? '') }, [value])
  const disabled = busy || value === undefined
  const number = ['float', 'int', 'uint'].includes(setting.type)
  const bounded = number && setting.min !== undefined && setting.max !== undefined && setting.max > setting.min
  const numericValue = Number(draft)
  const numericValid = draft.trim() !== '' && Number.isFinite(numericValue) &&
    (setting.type === 'float' || Number.isInteger(numericValue)) &&
    (setting.type !== 'uint' || numericValue >= 0) &&
    (setting.min === undefined || numericValue >= setting.min) &&
    (setting.max === undefined || numericValue <= setting.max)
  const commit = () => {
    if (!numericValid) { setDraft(value ?? ''); return }
    if (draft !== value) void onUpdate(setting, draft)
  }

  return <div className="snt-device-control">
    <div className="snt-device-control-heading">
      <label htmlFor={`snt-setting-${setting.key}`}>{label}</label>
      {!setting.persist && <span title="The device resets this setting on reboot">Session only</span>}
    </div>
    {setting.type === 'bool' && <button type="button" id={`snt-setting-${setting.key}`}
      className={`snt-toggle ${value === '1' ? 'active' : ''}`} role="switch"
      aria-label={label} aria-checked={value === '1'} disabled={disabled}
      onClick={() => void onUpdate(setting, value === '1' ? '0' : '1')}>
      <span className="snt-toggle-track"><span /></span><span>{value === '1' ? 'On' : 'Off'}</span>
    </button>}
    {setting.type === 'enum' && <select id={`snt-setting-${setting.key}`}
      disabled={disabled || !(setting.options?.length)} value={value ?? ''}
      onChange={event => void onUpdate(setting, event.currentTarget.value)}>
      {!setting.options?.includes(value ?? '') && <option value={value ?? ''}>{value || 'Unavailable'}</option>}
      {(setting.options ?? []).map(option => <option value={option} key={option}>
        {option.replaceAll('_', ' ').replace(/\b\w/g, c => c.toUpperCase())}
      </option>)}
    </select>}
    {number && <div className="snt-number-control">
      {bounded && <input aria-label={`${label} slider`} type="range"
        min={setting.min} max={setting.max}
        step={setting.type === 'float' ? Math.max(0.000001, (setting.max! - setting.min!) / 1000) : 1}
        disabled={disabled} value={numericValid ? draft : (value ?? setting.min)}
        onChange={event => setDraft(event.currentTarget.value)}
        onPointerUp={commit} onKeyUp={event => {
          if (event.key.startsWith('Arrow') || event.key === 'Home' || event.key === 'End') commit()
        }} onBlur={commit} />}
      <input id={`snt-setting-${setting.key}`} aria-label={`${label} number`} type="number"
        min={setting.min} max={setting.max} step={setting.type === 'float' ? 'any' : 1}
        disabled={disabled} value={draft} onChange={event => setDraft(event.currentTarget.value)}
        onBlur={commit} onKeyDown={event => { if (event.key === 'Enter') event.currentTarget.blur() }} />
    </div>}
  </div>
}

export function GlobalDeviceSettings({ connected }: GlobalProps) {
  const [schema, setSchema] = useState<Setting[]>([])
  const [values, setValues] = useState<Record<string, string>>({})
  const [loading, setLoading] = useState(connected)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  const [velocity, setVelocity] = useState<ResponseCurve>({ ...CURVE_PRESETS.linear })
  const [aftertouch, setAftertouch] = useState<ResponseCurve>({ ...CURVE_PRESETS.linear })

  const refresh = useCallback(async () => {
    if (!connected) return
    setLoading(true)
    setError('')
    try {
      const schemaLines = await window.tiles.usb.command('SCHEMA', true)
      const listLines = await window.tiles.usb.command('LIST', true)
      setSchema(readSchema(schemaLines))
      setValues(readValues(listLines))
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason))
      setSchema([])
      setValues({})
    } finally { setLoading(false) }
  }, [connected])

  useEffect(() => { void refresh() }, [refresh])
  useEffect(() => {
    function changed(event: Event) {
      const detail = (event as CustomEvent<{ key: string; value: string }>).detail
      if (detail?.key && typeof detail.value === 'string') {
        setValues(current => ({ ...current, [detail.key]: detail.value }))
      }
    }
    window.addEventListener('tiles:setting-changed', changed)
    return () => window.removeEventListener('tiles:setting-changed', changed)
  }, [])

  async function update(setting: Setting, next: string) {
    if (!connected || busy || /[\r\n\s]/.test(next) || /\s/.test(setting.key)) return
    if (setting.type === 'bool' && next !== '0' && next !== '1') return
    if (setting.type === 'enum' && !setting.options?.includes(next)) return
    if (['int', 'uint', 'float'].includes(setting.type)) {
      const n = Number(next)
      if (!next || !Number.isFinite(n) ||
        (setting.type !== 'float' && !Number.isInteger(n)) ||
        (setting.type === 'uint' && n < 0) ||
        (setting.min !== undefined && n < setting.min) ||
        (setting.max !== undefined && n > setting.max)) return
    }
    setBusy(true)
    setError('')
    try {
      const response = await window.tiles.usb.command(`SET ${setting.key} ${next}`)
      if (response[0] !== 'OK') throw new Error(response[0] || 'No SET response')
      const readback = await window.tiles.usb.command(`GET ${setting.key}`)
      if (!readback[0] || readback[0].startsWith('ERR ')) throw new Error(readback[0] || 'No GET response')
      setValues(current => ({ ...current, [setting.key]: readback[0] }))
      window.dispatchEvent(new CustomEvent('tiles:setting-changed', {
        detail: { key: setting.key, value: readback[0] },
      }))
    } catch (reason) { setError(`${setting.key}: ${String(reason)}`) }
    finally { setBusy(false) }
  }

  function show(key: string, label: string) {
    const setting = schema.find(s => s.key === key)
    if (!setting || !['bool', 'enum', 'int', 'uint', 'float'].includes(setting.type)) return null
    return <DeviceControl key={key} setting={setting} label={label}
      value={values[key]} busy={loading || busy} onUpdate={update} />
  }

  const hasPedal = schema.some(s => s.key.startsWith('pedal.'))
  const hasExpression = schema.some(s => s.key.startsWith('expression.'))

  return <div className="snt-global-settings">
    <div className="snt-panel-title-row">
      <div>
        <span className="snt-eyebrow">INSTRUMENT-WIDE</span>
        <h2>Global Device Settings</h2>
        <p>Parameters shared across playing modes.</p>
      </div>
      <button type="button" className="snt-subtle-button" disabled={!connected || busy || loading}
        onClick={() => void refresh()}>Refresh</button>
    </div>
    {connected && loading && <p className="snt-system-message">Reading firmware settings…</p>}
    {error && <p role="alert" className="snt-error">{error}</p>}
    {!connected && <p className="snt-system-message">Connect TILES to edit hardware parameters. Curves below are editable previews.</p>}

    <section className="snt-settings-section">
      <div className="snt-section-label"><h3>Pedal</h3><span>HARDWARE</span></div>
      {connected && hasPedal
        ? <div className="snt-control-stack">
            {show('pedal.mode', 'Pedal mode')}
            {show('pedal.polarity', 'Polarity')}
            {show('pedal.sustain_style', 'Sustain style')}
          </div>
        : <p className="snt-muted">{connected ? loading ? 'Loading…' : 'No pedal controls exposed by this firmware.' : 'Connect for pedal controls.'}</p>}
    </section>

    <section className="snt-settings-section">
      <div className="snt-section-label"><h3>Expression</h3><span>HARDWARE</span></div>
      {connected && hasExpression
        ? <div className="snt-control-stack">
            {show('expression.mpe_enabled', 'MPE enabled')}
            {show('expression.pitch_bend_sensitivity', 'Pitch bend sensitivity')}
            {show('expression.aftertouch_sensitivity', 'Aftertouch sensitivity')}
          </div>
        : <p className="snt-muted">{connected ? loading ? 'Loading…' : 'No expression controls exposed by this firmware.' : 'Connect for expression controls.'}</p>}
    </section>

    <section className="snt-settings-section">
      <div className="snt-section-label"><h3>Response curves</h3><span className="snt-preview-tag">UI PREVIEW</span></div>
      <p className="snt-muted">Shape how input pressure maps to output. These curves currently affect only the UI; they are not sent to firmware or saved on TILES.</p>
      <div className="snt-curve-stack">
        <ResponseCurveEditor label="Velocity curve" description="Initial strike response"
          value={velocity} onChange={setVelocity} />
        <ResponseCurveEditor label="Aftertouch curve" description="Continuous pressure response"
          value={aftertouch} onChange={setAftertouch} />
      </div>
    </section>

    <details className="snt-advanced">
      <summary>Advanced firmware settings <span>SCHEMA / LIST</span></summary>
      {connected ? <SchemaSettings /> : <p className="snt-muted">Connect TILES to view advanced parameters.</p>}
    </details>
  </div>
}
