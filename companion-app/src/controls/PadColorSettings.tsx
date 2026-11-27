import { useState } from 'react'
import type { CSSProperties, ReactNode } from 'react'
import type { DeviceSetting } from './DeviceControl'
import './PadColorSettings.css'
import { FloatingPanel } from './FloatingPanel'

const roleKeys = [
  'color.root',
  'color.fifth',
  'color.third',
]

const padKey = (index: number) =>
  `color.pad.${String(index).padStart(2, '0')}`

const isPad = (key: string) =>
  /^color\.pad\.(0[1-9]|1[0-9]|2[0-4])$/.test(key)

const isHex = (value: string | undefined): value is string =>
  /^[0-9a-f]{6}$/i.test(value ?? '')

export function PadColorSettings<S extends DeviceSetting>({
  settings,
  values,
  renderSetting,
  onUpdate,
  busy = false,
}: {
  settings: S[]
  values: Record<string, string>
  renderSetting: (setting: S) => ReactNode
  onUpdate: (setting: S, value: string) => Promise<void>
  busy?: boolean
}) {
  const [selected, setSelected] = useState(1)
  const [editorOpen, setEditorOpen] = useState(false)

  const byKey = new Map(
    settings.map(setting => [setting.key, setting])
  )

  const roles = roleKeys.flatMap(key => {
    const setting = byKey.get(key)
    return setting?.type === 'color' ? [setting] : []
  })

  const roleSet = new Set(
    roles.map(setting => setting.key)
  )

  const pads = settings.filter(
    setting => isPad(setting.key) && setting.type === 'color'
  )

  const padSet = new Set(
    pads.map(setting => setting.key)
  )

  const other = settings.filter(
    setting =>
      !roleSet.has(setting.key) &&
      !padSet.has(setting.key)
  )

  const overrides = pads.filter(
    setting => isHex(values[setting.key])
  ).length

  return (
    <div className="snt-pad-color-settings">

      {roles.length > 0 && (
        <div className="snt-role-color-grid">
          {roles.map(setting => (
            <div
              key={setting.key}
              className="snt-role-color-item"
            >
              {renderSetting(setting)}
            </div>
          ))}
        </div>
      )}

      {other.length > 0 && (
        <div className="snt-schema-grid snt-other-colors">
          {other.map(setting => (
            <div
              key={setting.key}
              className={
                setting.key === 'color.custom_pads'
                  ? 'snt-custom-pad-cell'
                  : ''
              }
            >
              {renderSetting(setting)}

              {setting.key === 'color.custom_pads' &&
                pads.length > 0 && (
                  <button
                    type="button"
                    className="snt-pad-palette-button"
                    title={`Customize individual pads (${overrides} overrides)`}
                    aria-label="Open Pad Color Editor"
                    aria-haspopup="dialog"
                    aria-expanded={editorOpen}
                    onClick={() => setEditorOpen(open => !open)}
                  >
                    <svg
                      width="16"
                      height="16"
                      viewBox="0 0 24 24"
                      fill="none"
                      stroke="currentColor"
                      strokeWidth="1.7"
                      strokeLinecap="round"
                      strokeLinejoin="round"
                      aria-hidden="true"
                    >
                      <path d="M12 3a9 9 0 1 0 0 18h1.5a1.5 1.5 0 0 0 1.1-2.5 1.5 1.5 0 0 1 1.1-2.5H18a3 3 0 0 0 3-3c0-5.5-4-10-9-10Z" />
                      <circle cx="7.5" cy="12" r="1" />
                      <circle cx="10" cy="7.5" r="1" />
                      <circle cx="15" cy="7.5" r="1" />
                      <circle cx="17.5" cy="12" r="1" />
                    </svg>
                  </button>
                )}
            </div>
          ))}
        </div>
      )}


      {pads.length > 0 && editorOpen && (
        <FloatingPanel
          title="Pad Color Editor"
          onClose={() => setEditorOpen(false)}
        >
          <div className="snt-schema-settings snt-pad-color-body snt-floating-pad-body">

            <div
              className="snt-mini-tiles"
              role="group"
              aria-label="Individual TILES pad colors"
            >
              <div className="snt-mini-tiles-top">
                <span>TILES</span>
                <span aria-hidden="true">
                  −　+　△　◇　□　○
                </span>
              </div>

              <div className="snt-mini-tiles-grid">
                {Array.from({ length: 24 }, (_, i) => {
                  const index = i + 1
                  const setting = byKey.get(padKey(index))

                  const value = setting
                    ? values[setting.key]
                    : undefined

                  const explicit = isHex(value)

                  const available =
                    !!setting &&
                    padSet.has(setting.key) &&
                    value !== undefined

                  const style = explicit
                    ? {
                        '--pad-color': `#${value}`,
                      } as CSSProperties
                    : undefined

                  return (
                    <div
                      key={index}
                      className={[
                        'snt-mini-pad',
                        explicit ? 'has-color' : 'inherits',
                        selected === index ? 'selected' : '',
                        !available ? 'unavailable' : '',
                      ].join(' ')}
                      style={style}
                      title={`Pad ${index} · ${
                        explicit ? `#${value}` : 'Inherited'
                      } · Right-click to inherit`}
                      onContextMenu={event => {
                        event.preventDefault()

                        if (!setting || !available || busy) return

                        setSelected(index)

                        void onUpdate(setting, 'none')
                      }}
                    >
                      <span>
                        {String(index).padStart(2, '0')}
                      </span>

                      {!explicit && available && (
                        <span
                          className="snt-mini-pad-inherit"
                          aria-hidden="true"
                        >
                          ↳
                        </span>
                      )}

                      <input
                        type="color"
                        className="snt-pad-native-picker"
                        aria-label={`Pad ${index} color`}
                        disabled={!available || busy}
                        value={explicit ? `#${value}` : '#ffffff'}
                        onClick={() => setSelected(index)}
                        onFocus={() => setSelected(index)}
                        onChange={event => {
                          if (!setting) return

                          void onUpdate(
                            setting,
                            event.currentTarget.value
                              .slice(1)
                              .toUpperCase()
                          )
                        }}
                        onKeyDown={event => {
                          if (
                            setting &&
                            available &&
                            !busy &&
                            event.key === 'Delete'
                          ) {
                            event.preventDefault()
                            void onUpdate(setting, 'none')
                          }
                        }}
                      />
                    </div>
                  )
                })}
              </div>
            </div>
          </div>
        </FloatingPanel>
      )}
    </div>
  )
}