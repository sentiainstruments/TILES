import { useCallback, useEffect, useState } from 'react'

type SettingType = 'bool' | 'enum' | 'int' | 'uint' | 'float' | 'color'
type Setting = {
  id: number
  key: string
  type: SettingType
  defaultValue: string
  persist: boolean
  min?: number
  max?: number
  options?: string[]
}

const TYPES = new Set<SettingType>(['bool', 'enum', 'int', 'uint', 'float', 'color'])

function assertNoError(lines: string[], command: string) {
  const error = lines.find(line => line.startsWith('ERR '))
  if (error) throw new Error(`${command}: ${error}`)
}

function parseSchema(lines: string[]): Setting[] {
  assertNoError(lines, 'SCHEMA')
  return lines.map(line => {
    const fields: Record<string, string> = {}
    for (const token of line.trim().split(/\s+/)) {
      const separator = token.indexOf('=')
      if (separator > 0) fields[token.slice(0, separator)] = token.slice(separator + 1)
    }
    const id = Number(fields.id)
    const type = fields.type as SettingType
    if (!fields.key || !Number.isInteger(id) || !TYPES.has(type) || fields.default === undefined) {
      throw new Error(`Invalid SCHEMA entry: ${line}`)
    }
    const min = fields.min === undefined ? undefined : Number(fields.min)
    const max = fields.max === undefined ? undefined : Number(fields.max)
    if ((min !== undefined && !Number.isFinite(min)) || (max !== undefined && !Number.isFinite(max))) {
      throw new Error(`Invalid range in SCHEMA: ${line}`)
    }
    return {
      id, key: fields.key, type, defaultValue: fields.default,
      persist: fields.persist !== '0', min, max,
      options: fields.values?.split('|'),
    }
  }).filter((entry, i, entries) => entries.findIndex(other => other.key === entry.key) === i)
}

function parseValues(lines: string[]) {
  assertNoError(lines, 'LIST')
  const result: Record<string, string> = {}
  for (const line of lines) {
    const separator = line.indexOf('=')
    if (separator > 0) result[line.slice(0, separator)] = line.slice(separator + 1)
  }
  return result
}

function title(text: string) {
  return text.replace(/_/g, ' ').replace(/\b\w/g, char => char.toUpperCase())
}

function SettingControl({ setting, value, busy, update, reset }: {
  setting: Setting
  value: string
  busy: boolean
  update: (setting: Setting, next: string) => Promise<void>
  reset: (setting: Setting) => Promise<void>
}) {
  const [draft, setDraft] = useState(value)
  useEffect(() => setDraft(value), [value])
  const numeric = ['int', 'uint', 'float'].includes(setting.type)
  const numberValue = Number(draft)
  const validNumber = draft.trim() !== '' && Number.isFinite(numberValue) &&
    (setting.type === 'float' || Number.isInteger(numberValue)) &&
    (setting.min === undefined || numberValue >= setting.min) &&
    (setting.max === undefined || numberValue <= setting.max)
  const commit = () => {
    if (!validNumber) { setDraft(value); return }
    if (draft !== value) void update(setting, draft)
  }
  const hex = /^[0-9a-f]{6}$/i.test(value)

  return <div style={{ display: 'grid', gridTemplateColumns: 'minmax(180px, 1fr) minmax(160px, 1fr) auto', gap: 14, alignItems: 'center', padding: '12px 0', borderBottom: '1px solid var(--border, #5553)' }}>
    <div>
      <div style={{ fontWeight: 600 }}>{title(setting.key.split('.').slice(1).join(' '))}</div>
      <small style={{ opacity: .65 }}>{setting.key}{!setting.persist ? ' · Resets on reboot' : ''}</small>
    </div>
    <div>
      {setting.type === 'bool' && <input aria-label={setting.key} type="checkbox" checked={value === '1'} disabled={busy} onChange={event => void update(setting, event.currentTarget.checked ? '1' : '0')} />}
      {setting.type === 'enum' && <select aria-label={setting.key} value={value} disabled={busy} onChange={event => void update(setting, event.currentTarget.value)}>
        {(setting.options ?? []).map(option => <option value={option} key={option}>{title(option)}</option>)}
      </select>}
      {numeric && <input aria-label={setting.key} type="number" value={draft} min={setting.min} max={setting.max} step={setting.type === 'float' ? 'any' : 1} disabled={busy} style={{ width: 130 }} onChange={event => setDraft(event.currentTarget.value)} onBlur={commit} onKeyDown={event => { if (event.key === 'Enter') event.currentTarget.blur() }} />}
      {setting.type === 'color' && <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        <input aria-label={setting.key} type="color" disabled={busy || !hex} value={hex ? `#${value}` : '#000000'} onChange={event => void update(setting, event.currentTarget.value.slice(1).toUpperCase())} />
        <label><input type="checkbox" disabled={busy} checked={value === 'none'} onChange={event => void update(setting, event.currentTarget.checked ? 'none' : (/^[0-9a-f]{6}$/i.test(setting.defaultValue) ? setting.defaultValue : 'FFFFFF'))} /> None</label>
      </div>}
    </div>
    <button type="button" disabled={busy || value === setting.defaultValue} onClick={() => void reset(setting)}>Reset</button>
  </div>
}

export function SchemaSettings() {
  const [schema, setSchema] = useState<Setting[]>([])
  const [values, setValues] = useState<Record<string, string>>({})
  const [loading, setLoading] = useState(true)
  const [busy, setBusy] = useState<string | null>(null)
  const [error, setError] = useState('')

  const refresh = useCallback(async () => {
    setLoading(true)
    setError('')
    try {
      // usb.command is serialized in the main process. These are protocol responses, not a local catalog.
      const schemaLines = await window.tiles.usb.command('SCHEMA', true)
      const listLines = await window.tiles.usb.command('LIST', true)
      const parsed = parseSchema(schemaLines)
      if (!parsed.length) throw new Error('The device returned an empty SCHEMA')
      setSchema(parsed)
      setValues(parseValues(listLines))
    } catch (reason) {
      setError(String(reason))
      setSchema([])
      setValues({})
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => { void refresh() }, [refresh])

  // Keep this global settings view synchronized when a contextual mode editor
  // changes a setting. The device remains the source of truth.
  useEffect(() => {
    function changed(event: Event) {
      const detail = (event as CustomEvent<{ key: string; value: string }>).detail
      if (detail && typeof detail.key === 'string' && typeof detail.value === 'string') {
        setValues(previous => ({ ...previous, [detail.key]: detail.value }))
      }
    }
    window.addEventListener('tiles:setting-changed', changed)
    return () => window.removeEventListener('tiles:setting-changed', changed)
  }, [])

  const publish = (key: string, value: string) => window.dispatchEvent(new CustomEvent('tiles:setting-changed', { detail: { key, value } }))

  async function update(setting: Setting, next: string) {
    setBusy(setting.key)
    setError('')
    try {
      const lines = await window.tiles.usb.command(`SET ${setting.key} ${next}`)
      if (lines[0] !== 'OK') throw new Error(`${setting.key}: ${lines[0] ?? 'No reply'}`)
      // Read back, as the instrument owns the authoritative value.
      const returned = await window.tiles.usb.command(`GET ${setting.key}`)
      assertNoError(returned, `GET ${setting.key}`)
      if (returned[0] === undefined) throw new Error(`No value returned for ${setting.key}`)
      setValues(previous => ({ ...previous, [setting.key]: returned[0] }))
      publish(setting.key, returned[0])
    } catch (reason) { setError(String(reason)) }
    finally { setBusy(null) }
  }

  async function reset(setting: Setting) {
    setBusy(setting.key)
    setError('')
    try {
      const lines = await window.tiles.usb.command(`RESET ${setting.key}`)
      if (lines[0] !== 'OK') throw new Error(`${setting.key}: ${lines[0] ?? 'No reply'}`)
      const returned = await window.tiles.usb.command(`GET ${setting.key}`)
      assertNoError(returned, `GET ${setting.key}`)
      if (returned[0] === undefined) throw new Error(`No value returned for ${setting.key}`)
      setValues(previous => ({ ...previous, [setting.key]: returned[0] }))
      publish(setting.key, returned[0])
    } catch (reason) { setError(String(reason)) }
    finally { setBusy(null) }
  }

  const grouped = schema.reduce<Record<string, Setting[]>>((result, setting) => {
    const group = setting.key.split('.')[0]
    ;(result[group] ??= []).push(setting)
    return result
  }, {})

  return <section style={{ padding: 24 }}>
    <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', gap: 12 }}>
      <div><h2>Device Settings</h2><p style={{ marginTop: 0 }}>Generated directly from the connected TILES firmware SCHEMA.</p></div>
      <button type="button" disabled={loading || busy !== null} onClick={() => void refresh()}>Refresh from TILES</button>
    </div>
    {loading && <p>Reading SCHEMA and LIST…</p>}
    {error && <p role="alert">{error}</p>}
    {!loading && Object.entries(grouped).map(([group, settings]) => <fieldset key={group} style={{ marginBottom: 20, padding: 18 }}>
      <legend style={{ fontWeight: 700, fontSize: 18 }}>{title(group)}</legend>
      {settings.map(setting => <SettingControl key={setting.id} setting={setting} value={values[setting.key] ?? ''} busy={busy !== null || values[setting.key] === undefined} update={update} reset={reset} />)}
    </fieldset>)}
    {!loading && schema.length > 0 && <small>{schema.length} settings reported by firmware. Settings apply live and are automatically saved by the device unless marked nonpersistent.</small>}
  </section>
}
