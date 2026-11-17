import { useCallback, useEffect, useMemo, useState } from 'react'
import './MelodicEditor.css'

type SettingType = 'bool' | 'enum' | 'int' | 'uint' | 'float' | 'color'
type Setting = {
  id: number
  key: string
  type: SettingType
  defaultValue: string
  persist: boolean
  options?: string[]
  min?: number
  max?: number
}

const ALLOWED_TYPES = new Set<SettingType>(['bool', 'enum', 'int', 'uint', 'float', 'color'])

// The keys here determine UI *organization*, not firmware capabilities.
// Available controls, their type, options and validation all come from SCHEMA.
const roleOrder = ['color.root', 'color.third', 'color.fifth', 'color.note', 'color.accidental']
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
    if (!Number.isInteger(id) || !fields.key || !ALLOWED_TYPES.has(type) || fields.default === undefined) {
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

function ColorEditor({ value, disabled, onChange, label }: {
  value: string
  disabled: boolean
  onChange: (value: string) => Promise<void>
  label: string
}) {
  const [draft, setDraft] = useState(isHex(value) ? value : 'FFFFFF')
  const [lastHex, setLastHex] = useState(isHex(value) ? value : 'FFFFFF')
  const [dirty, setDirty] = useState(false)

  useEffect(() => {
    if (isHex(value)) {
      setDraft(value)
      setLastHex(value)
    }
    setDirty(false)
  }, [value])

  // The native picker fires repeatedly while dragging. Commit only after
  // a quiet interval, so firmware flash autosave isn't continually postponed.
  useEffect(() => {
    if (!dirty || disabled || !isHex(draft)) return
    const timeout = window.setTimeout(() => {
      setDirty(false)
      if (draft.toUpperCase() !== value.toUpperCase()) void onChange(draft.toUpperCase())
    }, 300)
    return () => window.clearTimeout(timeout)
  }, [draft, dirty, disabled, value, onChange])

  return <div className="melodic-color-edit">
    <input type="color" aria-label={`${label} color`} disabled={disabled}
      value={`#${draft}`} onChange={event => {
        setDraft(event.currentTarget.value.slice(1).toUpperCase())
        setDirty(true)
      }} />
    <span className="melodic-color-hex">{value === 'none' ? 'Inherit / none' : `#${value}`}</span>
    <label className="melodic-checkbox-label">
      <input type="checkbox" checked={value === 'none'} disabled={disabled}
        onChange={event => void onChange(event.currentTarget.checked ? 'none' : lastHex)} />
      None
    </label>
  </div>
}

function NumberEditor({ setting, value, disabled, onChange }: {
  setting: Setting
  value: string
  disabled: boolean
  onChange: (value: string) => Promise<void>
}) {
  const [draft, setDraft] = useState(value)
  useEffect(() => setDraft(value), [value])
  const n = Number(draft)
  const valid = draft.trim() !== '' && Number.isFinite(n) &&
    (setting.type === 'float' || Number.isInteger(n)) &&
    (setting.type !== 'uint' || n >= 0) &&
    (setting.min === undefined || n >= setting.min) &&
    (setting.max === undefined || n <= setting.max)

  const commit = () => {
    if (!valid) { setDraft(value); return }
    if (draft !== value) void onChange(draft)
  }
  const showSlider = setting.min !== undefined && setting.max !== undefined &&
    setting.max > setting.min && setting.max - setting.min <= 2000
  const step = setting.type === 'float'
    ? Math.max(0.000001, ((setting.max ?? 1) - (setting.min ?? 0)) / 500)
    : 1
  return <div className="melodic-number-edit">
    {showSlider && <input type="range" aria-label={`${setting.key} slider`}
      min={setting.min} max={setting.max} step={step} disabled={disabled}
      value={valid ? draft : value}
      onChange={event => setDraft(event.currentTarget.value)}
      onPointerUp={commit}
      onKeyUp={event => { if (event.key === 'ArrowRight' || event.key === 'ArrowLeft') commit() }}
      onBlur={commit} />}
    <input type="number" aria-label={setting.key} disabled={disabled}
      min={setting.min} max={setting.max} step={setting.type === 'float' ? 'any' : 1}
      value={draft} onChange={event => setDraft(event.currentTarget.value)}
      onBlur={commit} onKeyDown={event => { if (event.key === 'Enter') event.currentTarget.blur() }} />
  </div>
}

function SettingRow({ setting, value, disabled, update, reset, caption }: {
  setting: Setting
  value: string | undefined
  disabled: boolean
  update: (setting: Setting, value: string) => Promise<void>
  reset: (setting: Setting) => Promise<void>
  caption?: string
}) {
  const present = value !== undefined
  const isBusy = disabled || !present
  const shownValue = value ?? ''
  return <div className="melodic-setting-row">
    <div className="melodic-setting-label">
      <strong>{title(setting.key)}</strong>
      <small title={setting.key}>{caption ?? setting.key}{!setting.persist ? ' · Resets on reboot' : ''}</small>
    </div>
    <div className="melodic-setting-input">
      {setting.type === 'bool' && <input type="checkbox" aria-label={setting.key}
        checked={shownValue === '1'} disabled={isBusy}
        onChange={event => void update(setting, event.currentTarget.checked ? '1' : '0')} />}
      {setting.type === 'enum' && <select aria-label={setting.key}
        value={shownValue} disabled={isBusy}
        onChange={event => void update(setting, event.currentTarget.value)}>
        {(setting.options ?? []).map(option => <option key={option} value={option}>
          {option.replace(/_/g, ' ').replace(/\b\w/g, c => c.toUpperCase())}
        </option>)}
      </select>}
      {['uint', 'int', 'float'].includes(setting.type) && <NumberEditor
        setting={setting} value={shownValue} disabled={isBusy}
        onChange={next => update(setting, next)} />}
      {setting.type === 'color' && <ColorEditor
        label={setting.key} value={shownValue} disabled={isBusy}
        onChange={next => update(setting, next)} />}
    </div>
    <button type="button" className="melodic-reset" title={`Reset ${setting.key}`}
      disabled={isBusy || shownValue.toLowerCase() === setting.defaultValue.toLowerCase()}
      onClick={() => void reset(setting)}>Reset</button>
  </div>
}

export function MelodicEditor({ selectedPad, onSelectPad }: {
  selectedPad: number | null
  onSelectPad: (pad: number) => void
}) {
  const [schema, setSchema] = useState<Setting[]>([])
  const [values, setValues] = useState<Record<string, string>>({})
  const [loading, setLoading] = useState(true)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  const [isMock, setIsMock] = useState(false)

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
  const roleColors = roleOrder.flatMap(key => byKey(key) ? [byKey(key)!] : [])
  const appearance = all(setting => setting.key.startsWith('look.') ||
    (setting.key.startsWith('color.') && setting.key !== 'color.custom_pads' &&
     !setting.key.startsWith('color.pad.') && !roleOrder.includes(setting.key)))
  const expression = all(setting => setting.key.startsWith('expression.'))
  const harmonics = all(setting => setting.key === 'features.melodic_harmonics' ||
    setting.key.startsWith('features.harmonics.'))
  const overrideKey = selectedPad ? `color.pad.${String(selectedPad).padStart(2, '0')}` : null
  const customEnabled = values['color.custom_pads'] === '1'

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
  const renderSetting = (setting: Setting, caption?: string) => <SettingRow
    key={setting.id} setting={setting} value={values[setting.key]}
    disabled={loading || busy} update={update} reset={reset} caption={caption} />

  return <aside className="melodic-editor" aria-label="Melodic mode customization">
    <div className="melodic-editor-heading">
      <div><div className="melodic-eyebrow">MODE 01</div><h3>Melodic</h3>
      </div>
      <button type="button" onClick={() => void refresh()} disabled={loading || busy}>Refresh</button>
    </div>
    <div className="melodic-status"><span className="melodic-status-dot" />
      {loading ? 'Reading TILES schema…' : isMock ? 'Mock board · no persistent writes' : 'Live device settings'}
    </div>
    {error && <div className="melodic-error" role="alert">{error}</div>}
    {!loading && !schema.length && <p>Device schema unavailable. No edits are enabled.</p>}
    {!!schema.length && <>
      <div className="melodic-editor-scroll">
        <section className="melodic-editor-group">
          <h4>Note layout</h4>
          {byKey('layout.melodic')
            ? renderSetting(byKey('layout.melodic')!, 'Changes the playable note arrangement')
            : <p className="melodic-muted">This firmware does not expose layout.melodic.</p>}
        </section>

        {!!roleColors.length && <section className="melodic-editor-group">
          <h4>Scale-role colors</h4>
          <p className="melodic-muted">The color associated with each note role.</p>
          {roleColors.map(setting => renderSetting(setting))}
        </section>}

        {(byKey('color.custom_pads') || overrideKey && byKey(overrideKey)) &&
          <section className="melodic-editor-group">
            <h4>Individual pad colors</h4>
            {byKey('color.custom_pads') && renderSetting(byKey('color.custom_pads')!,
              'Override role colors with pad-specific colors')}
            {schema.some(setting => setting.key.startsWith('color.pad.')) && <>
              <label className="melodic-picker-label">Choose pad
                <select value={selectedPad ?? ''} onChange={event => {
                  const pad = Number(event.currentTarget.value)
                  if (pad >= 1 && pad <= 24) onSelectPad(pad)
                }}>
                  <option value="">Select a pad…</option>
                  {Array.from({length: 24}, (_, i) => i + 1).map(n =>
                    <option key={n} value={n}>Pad {n}</option>)}
                </select>
              </label>
              {overrideKey && byKey(overrideKey) ? <div className={!customEnabled ? 'melodic-disabled-note' : ''}>
                {renderSetting(byKey(overrideKey)!, `Pad ${selectedPad} · ${customEnabled ? 'Override active' : 'Enable custom pads to see override'}`)}
              </div> : <p className="melodic-muted">Click a pad on the visualizer to edit its color.</p>}
            </>}
          </section>}

        {!!appearance.length && <section className="melodic-editor-group">
          <h4>Lighting and effects</h4>
          <p className="melodic-muted">Some lighting parameters are shared across modes.</p>
          {appearance.map(setting => renderSetting(setting))}
        </section>}

        {!!expression.length && <section className="melodic-editor-group">
          <h4>Expression</h4>
          <p className="melodic-muted">MPE, bend and pressure settings may affect other modes too.</p>
          {expression.map(setting => renderSetting(setting))}
        </section>}

        {!!harmonics.length && <section className="melodic-editor-group">
          <h4>Harmonics</h4>
          {harmonics.map(setting => renderSetting(setting))}
        </section>}
      </div>
      <p className="melodic-footer">Controls are discovered from SCHEMA. Each change uses SET and GET. The
        firmware decides which settings persist. Preview scale, key and mode are not sent to hardware.</p>
    </>}
  </aside>
}
