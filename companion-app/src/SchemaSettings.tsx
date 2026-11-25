import { PadColorSettings } from './controls/PadColorSettings'
import { DeviceControl } from './controls/DeviceControl'
import './SchemaSettings.css'
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

  return <section className="snt-schema-settings">

    {loading && <p className="snt-muted">Reading SCHEMA and LIST…</p>}
    {error && <p className="snt-error" role="alert">{error}</p>}
    {!loading && Object.entries(grouped).map(([group, settings]) => <section key={group} className="snt-schema-group">
      <h3>{title(group)}</h3>
      {group === 'color' ? <PadColorSettings settings={settings} values={values}  onUpdate={update}  busy={busy !== null || loading}
        renderSetting={setting => <div key={setting.key} className={`snt-schema-card ${['int', 'uint', 'float'].includes(setting.type) ? 'numeric' : ''}`}>
          <DeviceControl setting={setting} label={title(setting.key.split('.').slice(1).join(' ') || setting.key)}
            value={values[setting.key]} busy={busy !== null || loading}
            onUpdate={async (_, next) => { await update(setting, next) }}   onReset={() => reset(setting)}/>

        </div>} /> : <div className={`snt-schema-grid ${group === 'modes' ? 'snt-modes-grid' : ''}`}>
        {settings.map(setting => <div key={setting.key} className={`snt-schema-card ${['int', 'uint', 'float'].includes(setting.type) ? 'numeric' : ''}`}>
          <DeviceControl setting={setting} label={title(setting.key.split('.').slice(1).join(' ') || setting.key)}
            value={values[setting.key]} busy={busy !== null || loading}
            onUpdate={async (_, next) => { await update(setting, next) }}  onReset={() => reset(setting)} />

        </div>)}
      </div>}
    </section>)}
    {!loading && schema.length > 0 && <small className="snt-schema-count">{schema.length} settings reported by firmware. Persistence depends on the setting and firmware.</small>}
  </section>
}
