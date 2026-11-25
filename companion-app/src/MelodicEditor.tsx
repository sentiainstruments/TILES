import { DeviceControl } from './controls/DeviceControl'
import { PadColorSettings } from './controls/PadColorSettings'
import './SchemaSettings.css'
import './MelodicCompact.css'
import { useCallback, useEffect, useMemo, useState } from 'react'
import './MelodicEditor.css'

type SettingType = 'bool' | 'enum' | 'int' | 'uint' | 'float' | 'color'
type Setting = {
  id: number
  key: string
  type: SettingType
  defaultValue?: string
  persist: boolean
  options?: string[]
  min?: number
  max?: number
}

const ALLOWED_TYPES = new Set<SettingType>(['bool', 'enum', 'int', 'uint', 'float', 'color'])

// The keys here determine UI *organization*, not firmware capabilities.
// Available controls, their type, options and validation all come from SCHEMA.
const title = (key: string) => (key.split('.').slice(1).join(' ') || key)
  .replace(/_/g, ' ').replace(/\b\w/g, letter => letter.toUpperCase())

function parseSchema(lines: string[]): Setting[] {
  if (lines.some(line => line.startsWith('ERR '))) throw new Error(lines.join('\n'))
  const output = lines.map(line => {
    const fields: Record<string, string> = {}
    for (const token of line.trim().split(/\s+/)) {
      const at = token.indexOf('=')
      if (at > 0) fields[token.slice(0, at)] = token.slice(at + 1)
    }
    const id = Number(fields.id)
    const type = fields.type as SettingType
    if (!Number.isInteger(id) || !fields.key || !ALLOWED_TYPES.has(type)) {
      throw new Error(`Malformed schema line: ${line}`)
    }
    const min = fields.min === undefined ? undefined : Number(fields.min)
    const max = fields.max === undefined ? undefined : Number(fields.max)
    if ((min !== undefined && !Number.isFinite(min)) || (max !== undefined && !Number.isFinite(max))) {
      throw new Error(`Invalid range for ${fields.key}`)
    }
    return {
      id,
      key: fields.key,
      type,
      defaultValue: fields.default,
      persist: fields.persist !== '0',
      options: fields.values?.split('|'),
      min,
      max,
    }
  })
  if (!output.length) throw new Error('SCHEMA returned no settings')
  if (new Set(output.map(entry => entry.key)).size !== output.length) {
    throw new Error('SCHEMA contains duplicate setting keys')
  }
  return output
}

function parseList(lines: string[]): Record<string, string> {
  const error = lines.find(line => line.startsWith('ERR '))
  if (error) throw new Error(`LIST: ${error}`)
  const values: Record<string, string> = {}
  for (const line of lines) {
    const at = line.indexOf('=')
    if (at > 0) values[line.slice(0, at)] = line.slice(at + 1)
  }
  return values
}

const isHex = (value: string) => /^[0-9A-F]{6}$/i.test(value)

export function MelodicEditor({ selectedPad, onEditPreset }: {
  selectedPad: number | null
  onEditPreset?: () => void
  onSelectPad: (pad: number) => void
}) {
  const [schema, setSchema] = useState<Setting[]>([])
  const [values, setValues] = useState<Record<string, string>>({})
  const [loading, setLoading] = useState(true)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  const [, setIsMock] = useState(false)

  const refresh = useCallback(async () => {
    setLoading(true)
    setError('')
    try {
      const schemaLines = await window.tiles.usb.command('SCHEMA', true)
      const valuesLines = await window.tiles.usb.command('LIST', true)
      const parsedSchema = parseSchema(schemaLines)
      const parsedValues = parseList(valuesLines)
      setSchema(parsedSchema)
      setValues(parsedValues)
      // Publish a fresh device snapshot so the grid (and other open editors)
      // reflect changes made with the physical buttons or during reconnect.
      for (const [key, value] of Object.entries(parsedValues)) {
        if (key.startsWith('color.') || key.startsWith('look.') ||
            key.startsWith('layout.') || key.startsWith('expression.') ||
            key.startsWith('modes.')) {
          window.dispatchEvent(new CustomEvent('tiles:setting-changed', {
            detail: { key, value },
          }))
        }
      }
      const info = await window.tiles.usb.command('INFO', true)
      setIsMock(info.includes('simulator=1'))
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason))
      setSchema([])
      setValues({})
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => { void refresh() }, [refresh])
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

  const schemaByKey = useMemo(() => new Map(schema.map(setting => [setting.key, setting])), [schema])
  const byKey = (key: string) => schemaByKey.get(key)
  const all = (predicate: (setting: Setting) => boolean) => schema.filter(predicate)
  const colors = all(setting => setting.key.startsWith('color.'))
  const appearance = all(setting => setting.key.startsWith('look.'))
  const expression = all(setting => setting.key.startsWith('expression.'))
  const harmonics = all(setting => setting.key === 'features.melodic_harmonics' ||
    setting.key.startsWith('features.harmonics.'))
  const selectedColor = selectedPad === null ? undefined : byKey(`color.pad.${String(selectedPad).padStart(2, '0')}`)
  const layout = byKey('layout.melodic')

  const publish = (key: string, value: string) => window.dispatchEvent(new CustomEvent(
    'tiles:setting-changed', { detail: { key, value } },
  ))
  async function change(setting: Setting, command: string) {
    setBusy(true)
    setError('')
    try {
      const result = await window.tiles.usb.command(command)
      if (result[0] !== 'OK') throw new Error(`${command}: ${result[0] ?? 'No reply'}`)
      const readback = await window.tiles.usb.command(`GET ${setting.key}`)
      if (!readback[0] || readback[0].startsWith('ERR ')) {
        throw new Error(`GET ${setting.key}: ${readback[0] ?? 'No reply'}`)
      }
      setValues(previous => ({ ...previous, [setting.key]: readback[0] }))
      publish(setting.key, readback[0])
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason))
    } finally { setBusy(false) }
  }
  async function update(setting: Setting, value: string) {
    // Guard against malformed values reaching the device. Firmware enforces ranges too.
    if (/\s/.test(setting.key) || /[\r\n]/.test(value)) return
    if (setting.type === 'enum' && !setting.options?.includes(value)) return
    if (setting.type === 'bool' && value !== '0' && value !== '1') return
    if (setting.type === 'color' && value !== 'none' && !isHex(value)) return
    if (['int', 'uint', 'float'].includes(setting.type)) {
      const numeric = Number(value)
      if (!value.trim() || !Number.isFinite(numeric) ||
        (setting.type !== 'float' && !Number.isInteger(numeric)) ||
        (setting.type === 'uint' && numeric < 0) ||
        (setting.min !== undefined && numeric < setting.min) ||
        (setting.max !== undefined && numeric > setting.max)) return
    }
    await change(setting, `SET ${setting.key} ${value}`)
  }
  const reset = async (setting: Setting) => change(setting, `RESET ${setting.key}`)
  const renderSetting = (setting: Setting) => <div key={setting.key} className="snt-melodic-control-cell">
    <DeviceControl setting={setting} label={title(setting.key)}
      value={values[setting.key]} busy={loading || busy}
      onUpdate={async (_, next) => { await update(setting, next) }}
      onReset={() => reset(setting)} />
  </div>

  const renderGroup = (label: string, entries: Setting[]) => entries.length > 0 && (
    <section className="snt-schema-group">
      <h4>{label}</h4>
      <div className="snt-schema-grid">
        {entries.map(renderSetting)}
      </div>
    </section>
  )

  return <aside className="melodic-editor snt-melodic-compact" aria-label="Melodic mode customization">
    <div className="melodic-editor-heading">
      <h3>Melodic</h3>
      <div className="snt-mode-preview-action"><button type="button" className="snt-subtle-button" onClick={() => void refresh()} disabled={loading || busy}>Refresh</button>
        {onEditPreset && <button type="button" className="snt-subtle-button" onClick={onEditPreset}>Edit Preset</button>}
      </div>
    </div>

    {error && <div className="melodic-error" role="alert">{error}</div>}
    {!loading && !schema.length && <p className="melodic-muted">Device schema unavailable. No edits are enabled.</p>}
    {!!schema.length && <div className="melodic-editor-scroll snt-schema-settings">
      {layout && renderGroup('Note layout', [layout])}
      {colors.length > 0 && <section className="snt-schema-group">
        <h4>Scale-role colors</h4>
        <PadColorSettings settings={colors} values={values} renderSetting={renderSetting}
          onUpdate={update} busy={loading || busy} />
        {selectedColor && <div className="snt-melodic-quick-pad snt-schema-grid">
          {renderSetting(selectedColor)}
        </div>}
      </section>}
      {renderGroup('Lighting and effects', appearance)}
      {renderGroup('Expression', expression)}
      {renderGroup('Harmonics', harmonics)}
    </div>}
  </aside>
}
