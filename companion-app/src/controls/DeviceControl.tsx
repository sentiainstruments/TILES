import { useEffect, useId, useRef, useState } from 'react'

export type DeviceSetting = {
  key: string
  type: 'bool' | 'enum' | 'int' | 'uint' | 'float' | 'color'
  options?: string[]
  min?: number
  max?: number
  persist: boolean
  defaultValue?: string
}

export function DeviceControl({ setting, label, value, busy, onUpdate,   onReset }: {
  setting: DeviceSetting
  label: string
  value: string | undefined
  busy: boolean
  onUpdate: (setting: DeviceSetting, next: string) => Promise<void>
  onReset?: () => void | Promise<void>
}) {
  const id = useId()
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
  const pending = useRef(false)
  const commit = () => {
    if (disabled || pending.current) return
    if (!numericValid) { setDraft(value ?? ''); return }
    if (draft !== value) {
      pending.current = true
      void onUpdate(setting, draft).finally(() => { pending.current = false })
    }
  }

  return <div className="snt-device-control">
    <div className="snt-device-control-heading">
      {onReset ? (
        <button
          type="button"
          className="snt-reset-label"
          title={`Reset ${label} to default`}
          aria-label={`Reset ${label} to default`}
          disabled={disabled || value === setting.defaultValue}
          onClick={() => void onReset()}
        >
          {label}
        </button>
      ) : (
        <label htmlFor={id}>{label}</label>
      )}
      {!setting.persist && <span title="The device resets this setting on reboot">Session only</span>}
    </div>
    {setting.type === 'bool' && <button type="button" id={id}
      className={`snt-toggle ${value === '1' ? 'active' : ''}`} role="switch"
      aria-label={label} aria-checked={value === '1'} disabled={disabled}
      onClick={() => void onUpdate(setting, value === '1' ? '0' : '1')}>
      <span className="snt-toggle-track"><span /></span><span>{value === '1' ? 'On' : 'Off'}</span>
    </button>}
    {setting.type === 'enum' && <select id={id}
      disabled={disabled || !(setting.options?.length)} value={value ?? ''}
      onChange={event => void onUpdate(setting, event.currentTarget.value)}>
      {!setting.options?.includes(value ?? '') && <option value={value ?? ''}>{value || 'Unavailable'}</option>}
      {(setting.options ?? []).map(option => <option value={option} key={option}>
        {option.replaceAll('_', ' ').replace(/\b\w/g, c => c.toUpperCase())}
      </option>)}
    </select>}
    {setting.type === 'color' && <div className="snt-color-control">
      <input type="color" id={id} disabled={disabled}
        value={/^[0-9a-f]{6}$/i.test(value ?? '') ? `#${value}` : '#ffffff'}
        onChange={event => void onUpdate(setting, event.currentTarget.value.slice(1).toUpperCase())} />
      <span>{value === 'none' ? 'Inherit / none' : value ? `#${value}` : 'Unavailable'}</span>

    </div>}
    {number && <div className="snt-number-control">
      {bounded && <input aria-label={`${label} slider`} type="range"
        min={setting.min} max={setting.max}
        step={setting.type === 'float' ? Math.max(0.000001, (setting.max! - setting.min!) / 1000) : 1}
        disabled={disabled} value={numericValid ? draft : (value ?? setting.min)}
        onChange={event => setDraft(event.currentTarget.value)}
        onPointerUp={commit} onKeyUp={event => {
          if (event.key.startsWith('Arrow') || event.key === 'Home' || event.key === 'End') commit()
        }} onBlur={commit} />}

    </div>}
  </div>
}

