import { useEffect, useState } from 'react'

/**
 * New firmware/board-bringup features, discovered from the connected
 * device's SCHEMA. The same page works with real hardware or the simulator.
 * Do not send values for unsupported settings.
 */
const GROUPS = [
  { title: 'Lighting and performance', prefix: 'look.', keys: [
    'look.pressure_hue_degrees',
    'look.bend_dim_percent',
    'look.led_intensity_percent',
    'look.play_effect',
  ] },
  { title: 'Musical layout', prefix: 'layout.', keys: ['layout.melodic'] },
  { title: 'MIDI and synchronization', prefix: 'midi.', keys: [
    'midi.clock_lead_ms', 'midi.parts_port',
  ] },
  { title: 'Sequencer', prefix: 'seq.', keys: ['seq.lock'] },
  { title: 'Mode menu visibility', prefix: 'modes.', keys: [
    'modes.duet', 'modes.sequencer', 'modes.drums', 'modes.song',
    'modes.prism', 'modes.bass', 'modes.ableton', 'modes.volca',
    'modes.bounce', 'modes.cradle', 'modes.sand',
    'modes.ripples', 'modes.orbits',
  ] },
] as const

type SchemaSetting = {
  key: string
  type: string
  min?: number
  max?: number
  values?: string[]
}
const NEW_KEYS = new Set<string>(GROUPS.flatMap(group => [...group.keys]))

function parseSchemaLine(line: string): SchemaSetting | null {
  const parts = Object.fromEntries(line.trim().split(/\s+/).map(part => {
    const i = part.indexOf('=')
    return [part.slice(0, i), part.slice(i + 1)]
  }))
  if (!NEW_KEYS.has(parts.key)) return null
  return {
    key: parts.key,
    type: parts.type,
    min: parts.min === undefined ? undefined : Number(parts.min),
    max: parts.max === undefined ? undefined : Number(parts.max),
    values: parts.values?.split('|'),
  }
}

function prettyName(key: string) {
  return key.split('.').slice(1).join(' ').replace(/_/g, ' ')
    .replace(/\b\w/g, match => match.toUpperCase())
}

function SettingRow(p: {
  schema: SchemaSetting
  value: string
  onSave: (key: string, value: string) => Promise<void>
  busy: boolean
}) {
  const { schema, value, onSave, busy } = p
  const [draft, setDraft] = useState(value)
  useEffect(() => { setDraft(value) }, [value])
  const save = async (valueToSave: string) => {
    if (valueToSave !== value) await onSave(schema.key, valueToSave)
  }

  return (
    <label style={{
      display: 'flex', flexWrap: 'wrap', alignItems: 'center',
      justifyContent: 'space-between', gap: 12, padding: '8px 0',
    }}>
      <span title={schema.key}>{prettyName(schema.key)}</span>
      {schema.type === 'bool' ? (
        <input type="checkbox" checked={value === '1'} disabled={busy}
          onChange={e => void save(e.currentTarget.checked ? '1' : '0')} />
      ) : schema.type === 'enum' ? (
        <select value={value} disabled={busy}
          onChange={e => void save(e.currentTarget.value)}>
          {(schema.values ?? []).map(option =>
            <option key={option} value={option}>{option.replace(/_/g, ' ')}</option>
          )}
        </select>
      ) : (
        <input type="number"
          min={schema.min} max={schema.max}
          step={schema.type === 'float' ? 'any' : 1}
          value={draft} disabled={busy}
          style={{ width: 100 }}
          onChange={e => setDraft(e.currentTarget.value)}
          onKeyDown={e => {
            if (e.key === 'Enter') {
              e.currentTarget.blur()
            }
          }}
          onBlur={() => void save(draft)} />
      )}
    </label>
  )
}

export function FirmwareControls() {
  const [schema, setSchema] = useState<SchemaSetting[]>([])
  const [values, setValues] = useState<Record<string, string>>({})
  const [loading, setLoading] = useState(true)
  const [busy, setBusy] = useState<string | null>(null)
  const [error, setError] = useState('')

  useEffect(() => {
    let cancelled = false
    const load = async () => {
      try {
        const [schemaLines, settingsLines] = await Promise.all([
          window.tiles.usb.command('SCHEMA', true),
          window.tiles.usb.command('LIST', true),
        ])
        if (cancelled) return
        const available = schemaLines
          .map(parseSchemaLine)
          .filter((setting): setting is SchemaSetting => setting !== null)
        const entries = settingsLines.filter(line => line.includes('='))
          .map(line => {
            const splitAt = line.indexOf('=')
            return [line.slice(0, splitAt), line.slice(splitAt + 1)] as const
          })
        setSchema(available)
        setValues(Object.fromEntries(entries))
      } catch (reason) {
        if (!cancelled) setError(String(reason))
      } finally {
        if (!cancelled) setLoading(false)
      }
    }
    void load()
    return () => { cancelled = true }
  }, [])

  async function save(key: string, value: string) {
    setBusy(key)
    setError('')
    try {
      const [response] = await window.tiles.usb.command(`SET ${key} ${value}`)
      if (response !== 'OK') throw new Error(`${key}: ${response ?? 'no response'}`)
      setValues(previous => ({ ...previous, [key]: value }))
      window.dispatchEvent(new CustomEvent('tiles:setting-changed', {
        detail: { key, value },
      }))
    } catch (reason) {
      setError(String(reason))
    } finally {
      setBusy(null)
    }
  }

  return (
    <section>
      <h2>Extended firmware controls</h2>
      {loading && <p>Reading available firmware settings…</p>}
      {!loading && schema.length === 0 &&
        <p>The connected firmware does not expose the new board-bringup controls.</p>}
      {GROUPS.map(group => {
        const available = group.keys
          .map(key => schema.find(setting => setting.key === key))
          .filter((setting): setting is SchemaSetting => setting !== undefined)
        if (!available.length) return null
        return (
          <fieldset key={group.title} style={{ maxWidth: 650, marginBottom: 18 }}>
            <legend>{group.title}</legend>
            {available.map(setting => (
              <SettingRow key={setting.key} schema={setting}
                value={values[setting.key] ?? ''} busy={busy !== null}
                onSave={save} />
            ))}
          </fieldset>
        )
      })}
      {schema.length > 0 && <p>
        Showing {schema.length} additional controls reported by SCHEMA.
        Settings apply through the USB protocol; animated lighting and
        non-row layouts are not yet physically simulated.
      </p>}
      {error && <p role="alert">Error: {error}</p>}
    </section>
  )
}
