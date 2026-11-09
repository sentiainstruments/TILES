import { useEffect, useMemo, useState } from 'react'
import { PresetVisualizer } from './PresetVisualizer'
import {
  createPreset, createProject, validateProject, type TilesProject,
  type TilesPreset, type CustomScale,
} from './model'
import {
  defaultSettings, groups, niceLabel, settingCatalog,
  validForSchema, type SettingDescriptor,
} from './catalog'
import { applyReviewed, reviewPreset, type ApplyPlan } from './deviceApply'
import './ProjectWorkspace.css'

const ALL = settingCatalog

function updatePreset(project: TilesProject, id: string, fn: (p: TilesPreset) => TilesPreset): TilesProject {
  return { ...project, presets: project.presets.map(p => p.id === id ? fn(p) : p) }
}

type NameAction = 'project' | 'preset' | 'duplicate' | 'rename'

function SettingEditor(p: {
  descriptor: SettingDescriptor
  value: string
  onChange: (value: string) => void
}) {
  const { descriptor: field, value, onChange } = p
  const [draft, setDraft] = useState(value)
  useEffect(() => setDraft(value), [value])
  const saveNumber = () => {
    if (validForSchema(field, draft)) onChange(draft)
    else setDraft(value)
  }
  const pretty = niceLabel(field.key)
  if (field.type === 'bool') {
    return <label className="pw-setting" title={field.key}>
      <span>{pretty}</span>
      <input type="checkbox" checked={value === '1'}
        onChange={e => onChange(e.currentTarget.checked ? '1' : '0')} />
    </label>
  }
  if (field.type === 'enum') {
    return <label className="pw-setting" title={field.key}>
      <span>{pretty}</span>
      <select value={value} onChange={e => onChange(e.currentTarget.value)}>
        {(field.values ?? []).map(item =>
          <option key={item} value={item}>{item.replace(/_/g, ' ')}</option>)}
      </select>
    </label>
  }
  if (field.type === 'color') {
    const active = value !== 'none'
    return <label className="pw-setting" title={field.key}>
      <span>{pretty}</span>
      <span className="pw-color-row">
        <input type="color" value={active && /^[A-Fa-f0-9]{6}$/.test(value) ? `#${value}` : '#000000'}
          disabled={!active} onChange={e => onChange(e.currentTarget.value.slice(1).toUpperCase())} />
        <span className="pw-none"><input type="checkbox" checked={!active}
          onChange={e => onChange(e.currentTarget.checked ? 'none' : 'FFFFFF')} /> None</span>
      </span>
    </label>
  }
  return <label className="pw-setting" title={field.key}>
    <span>{pretty}</span>
    <span className="pw-number-row">
      <input type="number" value={draft} min={field.min} max={field.max}
        step={field.type === 'float' ? 'any' : '1'} onChange={e => setDraft(e.currentTarget.value)}
        onBlur={saveNumber} onKeyDown={e => { if (e.key === 'Enter') e.currentTarget.blur() }} />
      <small>{field.min}–{field.max}</small>
    </span>
  </label>
}

function ScalesEditor(p: { preset: TilesPreset; change: (scales: CustomScale[]) => void;
  choosePreview: (slot: number | null) => void }) {
  const { preset, change, choosePreview } = p
  const [slot, setSlot] = useState(1)
  const active = preset.scales.find(scale => scale.slot === slot)
  const [name, setName] = useState(active?.name ?? 'My_Scale')
  const [intervals, setIntervals] = useState(active?.intervals.join(',') ?? '0,2,4,5,7,9,11')
  const [message, setMessage] = useState('')
  useEffect(() => {
    setName(active?.name ?? 'My_Scale')
    setIntervals(active?.intervals.join(',') ?? '0,2,4,5,7,9,11')
    setMessage('')
  }, [active?.name, active?.intervals, slot])

  function saveScale() {
    const cleanName = name.trim().replace(/\s+/g, '_')
    const tokens = intervals.split(',').map(part => part.trim())
    const numbers = tokens.map(Number)
    const valid = tokens.every(t => /^\d+$/.test(t)) &&
      numbers.length >= 1 && numbers.length <= 12 && numbers[0] === 0 &&
      numbers.every((n, i) => n >= 0 && n <= 11 &&
        (i === 0 || n > numbers[i - 1]))
    if (!/^[!-~]{1,16}$/.test(cleanName) || !valid) {
      setMessage('Use a name of 1–16 ASCII characters and ascending intervals 0–11 starting with 0.')
      return
    }
    const updated = [...preset.scales.filter(scale => scale.slot !== slot),
      { slot, name: cleanName, intervals: numbers }].sort((a, b) => a.slot - b.slot)
    change(updated)
    choosePreview(slot)
    setMessage(`Scale saved in this preset (slot ${slot}).`)
  }
  return <section className="pw-scales">
    <p>Scales are stored in the project first. The hardware has nine custom slots.
      Choosing a preview scale does not select the active scale on the real TILES.</p>
    <label>Slot
      <select value={slot} onChange={e => setSlot(Number(e.currentTarget.value))}>
        {Array.from({ length: 9 }, (_, i) =>
          <option key={i} value={i + 1}>Slot {i + 1}{preset.scales.some(s => s.slot === i + 1) ? ' • saved' : ''}</option>)}
      </select>
    </label>
    <label>Scale name <input value={name} maxLength={16}
      onChange={e => setName(e.currentTarget.value)} /></label>
    <label>Semitone intervals <input value={intervals}
      onChange={e => setIntervals(e.currentTarget.value)} /></label>
    <div className="pw-inline-actions">
      <button type="button" onClick={saveScale}>Save scale to preset</button>
      <button type="button" disabled={!active} onClick={() => {
        change(preset.scales.filter(s => s.slot !== slot))
        if (preset.previewScaleSlot === slot) choosePreview(null)
      }}>Remove from preset</button>
    </div>
    <label>Preview scale
      <select value={preset.previewScaleSlot ?? ''} onChange={e =>
        choosePreview(e.currentTarget.value ? Number(e.currentTarget.value) : null)}>
        <option value="">Chromatic (default preview)</option>
        {preset.scales.map(scale =>
          <option key={scale.slot} value={scale.slot}>Slot {scale.slot}: {scale.name}</option>)}
      </select>
    </label>
    {message && <small role="status">{message}</small>}
  </section>
}

export function ProjectWorkspace() {
  const [project, setProject] = useState<TilesProject>(() => createProject('Untitled Project', defaultSettings))
  const [fileName, setFileName] = useState<string | null>(null)
  const [savedSnapshot, setSavedSnapshot] = useState<string | null>(null)
  const [section, setSection] = useState('Lighting')
  const [search, setSearch] = useState('')
  const [notice, setNotice] = useState('')
  const [error, setError] = useState('')
  const [working, setWorking] = useState(false)
  const [plan, setPlan] = useState<ApplyPlan | null>(null)
  const [planFor, setPlanFor] = useState('')
  const [uploadScales, setUploadScales] = useState(false)
  const [deviceStatus, setDeviceStatus] = useState('')
  const [naming, setNaming] = useState<{ action: NameAction; title: string } | null>(null)
  const [nameDraft, setNameDraft] = useState('')
  const selected = project.presets.find(p => p.id === project.selectedPresetId) ?? project.presets[0]
  const dirty = savedSnapshot === null || JSON.stringify(project) !== savedSnapshot
  const fields = useMemo(() => ALL.filter(field => {
    const group = groups.find(g => g.title === section)
    return field.key.startsWith(group?.prefix ?? '') &&
      (field.key.includes(search.toLowerCase()) || niceLabel(field.key).toLowerCase().includes(search.toLowerCase()))
  }), [section, search])

  const invalidatePlan = () => { setPlan(null); setPlanFor(''); setDeviceStatus('') }
  function editPreset(fn: (preset: TilesPreset) => TilesPreset) {
    setProject(p => updatePreset(p, p.selectedPresetId, fn))
    invalidatePlan()
  }
  function updateSetting(key: string, value: string) {
    editPreset(p => ({ ...p, settings: { ...p.settings, [key]: value } }))
  }
  function safeToDiscard() {
    return !dirty || window.confirm('Discard all unsaved changes to this project?')
  }

  function askForName(action: NameAction, title: string, initial: string) {
    setNaming({ action, title })
    setNameDraft(initial)
  }

  function newProject() {
    if (!safeToDiscard()) return
    askForName('project', 'New project', 'My TILES Project')
  }

  async function submitName() {
    const name = nameDraft.trim()
    if (!name || name.length > 100) {
      setError('Names must contain 1–100 characters.')
      return
    }
    if (!naming) return
    const action = naming.action
    setNaming(null)
    setError('')
    if (action === 'project') {
      try {
        await window.tiles.project.reset()
        setProject(createProject(name, defaultSettings))
        setFileName(null)
        setSavedSnapshot(null)
        setNotice('New project created. Save it to choose a location.')
        invalidatePlan()
      } catch (reason) { setError(String(reason)) }
      return
    }
    if (action === 'rename') {
      editPreset(p => ({ ...p, name }))
      return
    }
    if (project.presets.length >= 128) {
      setError('Projects support at most 128 presets.')
      return
    }
    const preset = action === 'duplicate'
      ? { ...selected, id: createPreset(name).id, name,
          settings: { ...selected.settings },
          scales: selected.scales.map(scale => ({ ...scale, intervals: [...scale.intervals] })) }
      : createPreset(name, defaultSettings)
    setProject(p => ({ ...p, selectedPresetId: preset.id,
      presets: [...p.presets, preset] }))
    invalidatePlan()
  }

  async function openProject() {
    if (!safeToDiscard()) return
    setWorking(true)
    try {
      const loaded = await window.tiles.project.open()
      if (!loaded) return
      const opened = validateProject(loaded.project)
      setProject(opened)
      setFileName(loaded.fileName)
      setSavedSnapshot(JSON.stringify(opened))
      setNotice(`Opened ${loaded.fileName}`)
      setError('')
      invalidatePlan()
    } catch (reason) { setError(String(reason)) }
    finally { setWorking(false) }
  }

  async function saveProject(saveAs = false) {
    setWorking(true)
    try {
      const next = { ...project, updatedAt: new Date().toISOString() }
      const saved = await window.tiles.project.save(next, saveAs)
      if (!saved) return
      setProject(next)
      setFileName(saved.fileName)
      setSavedSnapshot(JSON.stringify(next))
      setNotice(`Saved ${saved.fileName}`)
      setError('')
    } catch (reason) { setError(String(reason)) }
    finally { setWorking(false) }
  }

  function createNewPreset(copy = false) {
    askForName(copy ? 'duplicate' : 'preset',
      copy ? 'Duplicate preset' : 'New preset',
      copy ? `${selected.name} Copy` : 'New Preset')
  }

  function renamePreset() {
    askForName('rename', 'Rename preset', selected.name)
  }

  function deletePreset() {
    if (project.presets.length < 2) { setError('A project must keep at least one preset.'); return }
    if (!window.confirm(`Delete preset “${selected.name}” from this project?`)) return
    setProject(p => {
      const remaining = p.presets.filter(item => item.id !== p.selectedPresetId)
      return { ...p, presets: remaining, selectedPresetId: remaining[0].id }
    })
    invalidatePlan()
  }

  async function review() {
    setWorking(true)
    setNotice('')
    setError('')
    setPlan(null)
    setUploadScales(false)
    try {
      const next = await reviewPreset(selected)
      setPlan(next)
      setPlanFor(JSON.stringify(selected))
      setDeviceStatus(`Ready to review ${next.changes.length} setting changes.`)
    } catch (reason) { setError(`Could not read device: ${String(reason)}`) }
    finally { setWorking(false) }
  }

  async function apply() {
    if (!plan || planFor !== JSON.stringify(selected)) { setError('Review the preset again.'); return }
    if (plan.invalid.length) { setError('Fix invalid values before applying.'); return }
    setWorking(true)
    setError('')
    setNotice('')
    try {
      const result = await applyReviewed(plan, uploadScales, setDeviceStatus)
      setNotice(`Applied ${result.updated} settings and ${result.scalesWritten} scales. ` +
        `${result.alreadyMatching} settings already matched; ${result.skipped} unsupported settings skipped.`)
      setPlan(null)
    } catch (reason) {
      setError(`Apply stopped: ${String(reason)}. Changes made before the error may remain on TILES.`)
      setPlan(null)
    } finally { setWorking(false) }
  }

  return <section className="pw-root">
    {naming && <div className="pw-modal-backdrop" role="presentation">
      <form className="pw-modal" onSubmit={e => { e.preventDefault(); void submitName() }}
        role="dialog" aria-modal="true" aria-label={naming.title}>
        <h3>{naming.title}</h3>
        <label>Name
          <input autoFocus value={nameDraft} maxLength={100}
            onChange={e => setNameDraft(e.currentTarget.value)} />
        </label>
        <div className="pw-inline-actions">
          <button type="submit" disabled={!nameDraft.trim()}>Continue</button>
          <button type="button" onClick={() => setNaming(null)}>Cancel</button>
        </div>
      </form>
    </div>}
    <header className="pw-header">
      <div>
        <span className="pw-eyebrow">SENTIA / TILES · CREATIVE WORKSPACE</span>
        <div className="pw-heading">
          <input aria-label="Project name" value={project.name} maxLength={100}
            onChange={e => { const name = e.currentTarget.value; setProject(p => ({ ...p, name })); invalidatePlan() }} />
          <span className={dirty ? 'pw-status pw-dirty' : 'pw-status'}>
            {dirty ? 'Unsaved changes' : 'Saved'}</span>
        </div>
        <p>{fileName ?? 'New local project'} · {project.presets.length} presets · Saved to this computer only</p>
      </div>
      <div className="pw-toolbar">
        <button type="button" disabled={working} onClick={newProject}>New</button>
        <button type="button" disabled={working} onClick={() => void openProject()}>Open…</button>
        <button type="button" disabled={working} onClick={() => void saveProject()}>Save</button>
        <button type="button" disabled={working} onClick={() => void saveProject(true)}>Save as…</button>
      </div>
    </header>

    <div className="pw-layout">
      <aside className="pw-sidebar">
        <div className="pw-side-title"><h3>Preset library</h3><small>{project.presets.length} presets</small></div>
        <div className="pw-preset-list">
          {project.presets.map(preset => <button key={preset.id} type="button"
            className={`pw-preset-item ${selected.id === preset.id ? 'selected' : ''}`}
            onClick={() => { setProject(p => ({ ...p, selectedPresetId: preset.id })); invalidatePlan() }}>
            <span className="pw-swatch" style={{ background: `#${/^[0-9a-f]{6}$/i.test(preset.settings['color.root'] ?? '') ? preset.settings['color.root'] : '8A36C4'}` }} />
            <span><strong>{preset.name}</strong><small>{preset.description || 'Custom configuration'}</small></span>
          </button>)}
        </div>
        <div className="pw-side-actions">
          <button type="button" onClick={() => createNewPreset()}>+ New preset</button>
          <button type="button" onClick={() => createNewPreset(true)}>Duplicate</button>
          <button type="button" onClick={renamePreset}>Rename</button>
          <button type="button" onClick={deletePreset}>Delete</button>
        </div>
        <div className="pw-side-help">Changes are staged in this project.
          Save writes a local file. Apply sends a configuration to the device.</div>
      </aside>

      <div className="pw-main">
        <div className="pw-preset-heading">
          <div><span className="pw-eyebrow">EDITING PRESET</span><h2>{selected.name}</h2></div>
          <button className="pw-apply" type="button" disabled={working} onClick={() => void review()}>
            Review &amp; Apply to TILES</button>
        </div>
        <label className="pw-description">Notes about this preset
          <input value={selected.description} maxLength={1000}
            placeholder="e.g. Quiet pad colors, expressive lead layout"
            onChange={e => editPreset(p => ({ ...p, description: e.currentTarget.value }))} />
        </label>
        <PresetVisualizer settings={selected.settings} scales={selected.scales}
          previewScaleSlot={selected.previewScaleSlot} />

        <section className="pw-editor">
          <div className="pw-editor-title"><h3>Preset settings</h3>
            <span>{Object.keys(selected.settings).length} parameters saved in preset</span></div>
          <nav className="pw-categories" aria-label="Settings categories">
            {groups.map(group => <button key={group.title} type="button"
              className={section === group.title ? 'selected' : ''}
              onClick={() => setSection(group.title)}>{group.title}</button>)}
            <button type="button" className={section === 'Custom scales' ? 'selected' : ''}
              onClick={() => setSection('Custom scales')}>Custom scales</button>
          </nav>
          {section === 'Custom scales' ? <ScalesEditor preset={selected}
            change={scales => editPreset(p => ({ ...p, scales }))}
            choosePreview={previewScaleSlot => editPreset(p => ({ ...p, previewScaleSlot }))} /> : <>
            <input className="pw-search" type="search" placeholder="Search settings…"
              value={search} onChange={e => setSearch(e.currentTarget.value)} />
            <div className="pw-settings-grid">
              {fields.map(field => <SettingEditor key={field.key} descriptor={field}
                value={selected.settings[field.key] ?? field.default}
                onChange={value => updateSetting(field.key, value)} />)}
            </div>
            {!fields.length && <p>No settings match this search.</p>}
          </>}
        </section>

        {plan && <section className="pw-review" aria-label="Apply review">
          <h3>Review changes before applying</h3>
          <p>This operation will modify the connected TILES. Your local project is not automatically saved.</p>
          <div className="pw-review-stats">
            <span><strong>{plan.changes.length}</strong> settings to change</span>
            <span><strong>{plan.alreadyMatching}</strong> already match</span>
            <span><strong>{plan.unsupported.length}</strong> unsupported</span>
            <span><strong>{plan.invalid.length}</strong> invalid</span>
          </div>
          {plan.unsupported.length > 0 && <details><summary>Unsupported settings ({plan.unsupported.length})</summary>
            <p>{plan.unsupported.join(', ')}</p></details>}
          {plan.invalid.length > 0 && <p role="alert" className="pw-error">Invalid preset values: {plan.invalid.join(', ')}</p>}
          {plan.scales.length > 0 && <label className="pw-scale-confirm">
            <input type="checkbox" checked={uploadScales} disabled={!plan.scalesSupported}
              onChange={e => setUploadScales(e.currentTarget.checked)} />
            Also upload {plan.scales.length} custom scales (overwrites matching device slots)
          </label>}
          {uploadScales && plan.slotConflicts.length > 0 &&
            <p className="pw-warning">Existing device scales in slot(s) {plan.slotConflicts.join(', ')} will be replaced.</p>}
          <div className="pw-inline-actions">
            <button className="pw-apply" type="button" disabled={working || plan.invalid.length > 0}
              onClick={() => void apply()}>Apply {plan.changes.length} settings{uploadScales ? ' + scales' : ''}</button>
            <button type="button" onClick={() => setPlan(null)} disabled={working}>Cancel</button>
          </div>
        </section>}
        {deviceStatus && <p className="pw-info" role="status">{deviceStatus}</p>}
        {notice && <p className="pw-success" role="status">{notice}</p>}
        {error && <p className="pw-error" role="alert">{error}</p>}
      </div>
    </div>
  </section>
}
