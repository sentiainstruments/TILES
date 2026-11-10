import { useEffect, useState } from 'react'
import { useTilesSettings } from './settings'

const COLORS: Record<string, string> = {
  root: '660066',
  fifth: '240066',
  third: '663300',
  note: '363636',
  accidental: '000000',
}
const ROLE_KEYS = Object.fromEntries(
  Object.keys(COLORS).map(role => [role, `color.${role}`]),
)
const PAD_KEYS = Object.fromEntries(
  Array.from({ length: 24 }, (_, i) => {
    const pad = String(i + 1).padStart(2, '0')
    return [pad, `color.pad.${pad}`]
  }),
)
const CUSTOM_KEYS = { enabled: 'color.custom_pads' }
const COMMON_KEYS = {
  idle: 'look.idle_baseline_percent',
  sustainTint: 'look.echo_sustain_tint_percent',
  echoGreen: 'look.echo_secondary_g_percent',
  echoBlue: 'look.echo_secondary_b_percent',
  flashMs: 'look.echo_flash_ms',
}
const LEGACY_KEYS = {
  natural: 'look.natural_pad_percent',
  root: 'look.root_pad_percent',
  fifth: 'look.fifth_pad_percent',
  fifthRed: 'look.fifth_red_tint_percent',
}
const isColor = (value: string | undefined) =>
  value && /^[0-9a-f]{6}$/i.test(value)

function CommonLighting() {
  const { values, update, error } = useTilesSettings(COMMON_KEYS)
  return (
    <>
      <h3>Brightness and echo</h3>
      {([
        ['idle', 'Idle baseline'],
        ['sustainTint', 'Sustain tint'],
        ['echoGreen', 'Echo green'],
        ['echoBlue', 'Echo blue'],
      ] as const).map(([name, label]) => (
        <label key={name}>
          {label} ({values[name] ?? '…'}%)
          <input type="range" min="0" max="100"
            value={values[name] ?? '0'}
            onChange={e => void update(name, e.currentTarget.value)} />
        </label>
      ))}
      <label>
        Echo flash (ms)
        <input type="number" min="0" max="2000"
          value={values.flashMs ?? ''}
          onChange={e => void update('flashMs', e.currentTarget.value)} />
      </label>
      {error && <p role="alert">Error: {error}</p>}
    </>
  )
}

function LegacyLighting() {
  const { values, update, error } = useTilesSettings(LEGACY_KEYS)
  return (
    <>
      <h3>Legacy lighting (firmware 0.2.0)</h3>
      {([
        ['natural', 'Natural pad'],
        ['root', 'Root pad'],
        ['fifth', 'Fifth pad'],
        ['fifthRed', 'Fifth red tint'],
      ] as const).map(([name, label]) => (
        <label key={name}>
          {label} ({values[name] ?? '…'}%)
          <input type="range" min="0" max="100"
            value={values[name] ?? '0'}
            onChange={e => void update(name, e.currentTarget.value)} />
        </label>
      ))}
      {error && <p role="alert">Error: {error}</p>}
    </>
  )
}

function ModernLighting() {
  const roles = useTilesSettings(ROLE_KEYS)
  const pads = useTilesSettings(PAD_KEYS)
  const custom = useTilesSettings(CUSTOM_KEYS)

  return (
    <>
      <h3>Scale-role colors</h3>
      {Object.keys(COLORS).map(role => {
        const value = roles.values[role] ?? 'none'
        const off = value === 'none'
        return (
          <label key={role}>
            {role[0].toUpperCase() + role.slice(1)}{' '}
            <input type="color" disabled={off}
              value={isColor(value) ? `#${value}` : '#000000'}
              onChange={e => void roles.update(role, e.currentTarget.value.slice(1).toUpperCase())} />
            <input type="checkbox" checked={off}
              onChange={e => void roles.update(role, e.currentTarget.checked ? 'none' : COLORS[role])} />
            No highlight
          </label>
        )
      })}
      <h3>Individual pad colors</h3>
      <label>
        <input type="checkbox" checked={custom.values.enabled === '1'}
          onChange={e => void custom.update('enabled', e.currentTarget.checked ? '1' : '0')} />
        Override scale-role colors for individual pads
      </label>
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(6, minmax(0, 1fr))', gap: 8, maxWidth: 650 }}>
        {Object.keys(PAD_KEYS).map(pad => {
          const value = pads.values[pad] ?? 'none'
          return (
            <label key={pad} style={{ display: 'flex', flexDirection: 'column', gap: 4 }}>
              Pad {pad}
              <input type="color" disabled={custom.values.enabled !== '1'}
                value={isColor(value) ? `#${value}` : '#000000'}
                onChange={e => void pads.update(pad, e.currentTarget.value.slice(1).toUpperCase())} />
              <span style={{ fontSize: 11 }}>
                <input type="checkbox" disabled={custom.values.enabled !== '1'}
                  checked={value === 'none'}
                  onChange={e => void pads.update(pad, e.currentTarget.checked ? 'none' : 'FFFFFF')} />
                Inherit
              </span>
            </label>
          )
        })}
      </div>
      {(roles.error || pads.error || custom.error) &&
        <p role="alert">Error: {roles.error || pads.error || custom.error}</p>}
    </>
  )
}

export function Lighting() {
  const [mode, setMode] = useState<'loading' | 'modern' | 'legacy' | 'error'>('loading')

  useEffect(() => {
    let cancelled = false
    void window.tiles.usb.command('SCHEMA', true)
      .then(lines => {
        if (cancelled) return
        if (lines.some(line => line.includes('key=color.root '))) setMode('modern')
        else if (lines.some(line => line.includes('key=look.root_pad_percent '))) setMode('legacy')
        else setMode('error')
      })
      .catch(() => { if (!cancelled) setMode('error') })
    return () => { cancelled = true }
  }, [])

  return (
    <section>
      <h2>Lighting</h2>
      {mode === 'loading' && <p>Reading device lighting capabilities…</p>}
      {mode === 'error' && <p>Lighting capabilities are unavailable.</p>}
      {mode === 'modern' && <ModernLighting />}
      {mode === 'legacy' && <LegacyLighting />}
      {(mode === 'modern' || mode === 'legacy') && <CommonLighting />}
    </section>
  )
}
