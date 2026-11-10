import { equivalentValue, parseList, parseSchema, validForSchema, type SettingDescriptor } from './catalog'
import type { TilesPreset } from './model'

type SettingChange = { key: string; before: string; after: string; schema: SettingDescriptor }
export type ApplyPlan = {
  changes: SettingChange[]
  alreadyMatching: number
  unsupported: string[]
  invalid: string[]
  scales: TilesPreset['scales']
  scalesSupported: boolean
  slotConflicts: number[]
}

async function command(line: string, multi = false) {
  const lines = await window.tiles.usb.command(line, multi)
  if (lines.some(row => row.startsWith('ERR '))) {
    throw new Error(`${line}: ${lines.join('; ')}`)
  }
  return lines
}

export async function reviewPreset(preset: TilesPreset): Promise<ApplyPlan> {
  const [schemaRows, valueRows] = await Promise.all([
    command('SCHEMA', true), command('LIST', true),
  ])
  const schema = parseSchema(schemaRows)
  if (schema.size === 0) throw new Error('Connected device did not provide a usable SCHEMA.')
  const current = parseList(valueRows)
  const changes: SettingChange[] = []
  const unsupported: string[] = []
  const invalid: string[] = []
  let alreadyMatching = 0
  for (const [key, after] of Object.entries(preset.settings)) {
    const field = schema.get(key)
    if (!field || !current.has(key)) { unsupported.push(key); continue }
    if (!validForSchema(field, after)) { invalid.push(key); continue }
    const before = current.get(key) ?? ''
    if (equivalentValue(before, after, field.type)) { alreadyMatching++; continue }
    changes.push({ key, before, after, schema: field })
  }
  let scalesSupported = false
  const slotConflicts: number[] = []
  if (preset.scales.length) {
    const rows = await window.tiles.usb.command('SCALES', true)
    scalesSupported = !rows.some(row => row.startsWith('ERR '))
    if (scalesSupported) {
      for (const scale of preset.scales) {
        const old = rows.find(row => row.startsWith(`${scale.slot} `))
        if (old && !old.includes(` ${scale.name} ${scale.intervals.join(',')}`)) {
          slotConflicts.push(scale.slot)
        }
      }
    }
  }
  return { changes, unsupported, invalid, alreadyMatching,
    scales: preset.scales, scalesSupported, slotConflicts }
}

export type ApplyResult = {
  updated: number
  scalesWritten: number
  skipped: number
  alreadyMatching: number
}

export async function applyReviewed(
  plan: ApplyPlan,
  includeScales: boolean,
  onProgress: (text: string) => void,
): Promise<ApplyResult> {
  if (plan.invalid.length) throw new Error('Fix invalid settings before applying.')
  let updated = 0
  let scalesWritten = 0
  for (const change of plan.changes) {
    onProgress(`Applying ${change.key} (${updated + 1}/${plan.changes.length})`)
    const result = await command(`SET ${change.key} ${change.after}`)
    if (result[0] !== 'OK') throw new Error(`SET ${change.key}: ${result.join('; ')}`)
    const [actual] = await command(`GET ${change.key}`)
    if (!equivalentValue(actual ?? '', change.after, change.schema.type)) {
      throw new Error(`Could not verify ${change.key}: expected ${change.after}, got ${actual}`)
    }
    updated++
    window.dispatchEvent(new CustomEvent('tiles:setting-changed', {
      detail: { key: change.key, value: change.after },
    }))
  }
  if (includeScales) {
    if (!plan.scalesSupported) throw new Error('Device does not support custom scales.')
    for (const scale of plan.scales) {
      onProgress(`Uploading scale slot ${scale.slot}`)
      const cmd = `SCALE PUT ${scale.slot} ${scale.name} ${scale.intervals.join(',')}`
      const result = await command(cmd)
      if (result[0] !== 'OK') throw new Error(`Scale slot ${scale.slot}: ${result.join('; ')}`)
      const verify = await command(`SCALE GET ${scale.slot}`, true)
      if (!verify.some(row => row.startsWith(`${scale.slot} ${scale.name} ${scale.intervals.join(',')}`))) {
        throw new Error(`Scale slot ${scale.slot} was written but couldn't be verified.`)
      }
      scalesWritten++
    }
    if (scalesWritten) window.dispatchEvent(new Event('tiles:scales-changed'))
  }
  return { updated, scalesWritten, skipped: plan.unsupported.length,
    alreadyMatching: plan.alreadyMatching }
}
