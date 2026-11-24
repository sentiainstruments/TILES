import { useId, useRef } from 'react'
import type { KeyboardEvent, PointerEvent } from 'react'
import './ResponseCurveEditor.css'

/** Normalized output at 1/3 and 2/3 input. Endpoints are fixed at (0,0) and (1,1). */
export type ResponseCurve = { low: number; high: number }

export const CURVE_PRESETS: Record<string, ResponseCurve> = {
  linear: { low: 1 / 3, high: 2 / 3 },
  gentle: { low: 0.16, high: 0.47 },
  strong: { low: 0.53, high: 0.86 },
  sCurve: { low: 0.13, high: 0.87 },
}

type Props = {
  label: string
  description?: string
  value: ResponseCurve
  onChange: (next: ResponseCurve) => void
  disabled?: boolean
}

const clamp = (n: number, min: number, max: number) => Math.min(max, Math.max(min, n))
const near = (a: number, b: number) => Math.abs(a - b) < 0.006
const coordinates = [0, 1 / 3, 2 / 3, 1]

/** Monotone cubic Hermite interpolation, expressed as an SVG Bezier path. */
function getPath(curve: ResponseCurve): string {
  const y = [0, curve.low, curve.high, 1]
  const slopes = [0, 1, 2].map(i => (y[i + 1] - y[i]) / (coordinates[i + 1] - coordinates[i]))
  const tangent = [slopes[0], 0, 0, slopes[2]]
  for (let i = 1; i < 3; i++) {
    tangent[i] = Math.min((slopes[i - 1] + slopes[i]) / 2, 3 * Math.min(slopes[i - 1], slopes[i]))
  }
  const pt = (x: number, output: number) => `${(x * 100).toFixed(3)} ${(100 - output * 100).toFixed(3)}`
  let d = `M ${pt(0, 0)}`
  for (let i = 0; i < 3; i++) {
    const dx = (coordinates[i + 1] - coordinates[i]) / 3
    d += ` C ${pt(coordinates[i] + dx, y[i] + tangent[i] * dx)}`
    d += ` ${pt(coordinates[i + 1] - dx, y[i + 1] - tangent[i + 1] * dx)}`
    d += ` ${pt(coordinates[i + 1], y[i + 1])}`
  }
  return d
}

export function ResponseCurveEditor({ label, description, value, onChange, disabled = false }: Props) {
  const svgRef = useRef<SVGSVGElement>(null)
  const gradientId = `snt-curve-${useId().replace(/:/g, '')}`
  const preset = Object.entries(CURVE_PRESETS).find(([, p]) =>
    near(p.low, value.low) && near(p.high, value.high))?.[0] ?? 'custom'
  const path = getPath(value)

  function updateHandle(which: 'low' | 'high', next: number) {
    if (disabled) return
    const low = which === 'low'
      ? clamp(next, 0.02, value.high - 0.02)
      : value.low
    const high = which === 'high'
      ? clamp(next, value.low + 0.02, 0.98)
      : value.high
    onChange({ low, high })
  }

  function moveHandle(event: PointerEvent<SVGCircleElement>, which: 'low' | 'high') {
    if (disabled || !event.currentTarget.hasPointerCapture(event.pointerId)) return
    const bounds = svgRef.current?.getBoundingClientRect()
    if (!bounds) return
    updateHandle(which, 1 - (event.clientY - bounds.top) / bounds.height)
  }

  function keyHandle(event: KeyboardEvent<SVGCircleElement>, which: 'low' | 'high') {
    if (disabled) return
    const change = event.key === 'ArrowUp' || event.key === 'ArrowRight' ? 0.02
      : event.key === 'ArrowDown' || event.key === 'ArrowLeft' ? -0.02 : 0
    if (change) {
      event.preventDefault()
      updateHandle(which, value[which] + change)
    }
  }

  return <section className="snt-curve" aria-label={`${label} response curve`}>
    <div className="snt-curve-heading">
      <div>
        <h4>{label}</h4>
        {description && <p>{description}</p>}
      </div>
      <select aria-label={`${label} curve preset`} value={preset} disabled={disabled}
        onChange={event => {
          const next = CURVE_PRESETS[event.currentTarget.value]
          if (next) onChange({ ...next })
        }}>
        <option value="linear">Linear</option>
        <option value="gentle">Gentle</option>
        <option value="strong">Strong</option>
        <option value="sCurve">S-curve</option>
        {preset === 'custom' && <option value="custom">Custom</option>}
      </select>
    </div>
    <div className="snt-curve-chart">
      <span className="snt-curve-axis-y">Output</span>
      <svg ref={svgRef} viewBox="0 0 100 100" preserveAspectRatio="none" role="group"
        aria-label={`${label}: drag either handle vertically, or use arrow keys`}>
        <defs>
          <linearGradient id={gradientId} x1="0" y1="0" x2="0" y2="1">
            <stop offset="0%" stopColor="#cb9dff" stopOpacity="0.2" />
            <stop offset="100%" stopColor="#9a6ef5" stopOpacity="0" />
          </linearGradient>
        </defs>
        {[0, 25, 50, 75, 100].map(tick => <g key={tick} className="snt-curve-grid">
          <line x1={tick} x2={tick} y1="0" y2="100" />
          <line x1="0" x2="100" y1={tick} y2={tick} />
        </g>)}
        <line x1="0" y1="100" x2="100" y2="0" className="snt-curve-guide" />
        <path d={`${path} L 100 100 L 0 100 Z`}
          fill={`url(#${gradientId})`} />
        <path d={path} className="snt-curve-line" />
        {(['low', 'high'] as const).map((which, index) => <circle
          key={which}
          cx={(coordinates[index + 1] * 100).toFixed(3)}
          cy={((1 - value[which]) * 100).toFixed(3)}
          r="3.5"
          className="snt-curve-handle"
          tabIndex={disabled ? -1 : 0}
          role="slider"
          aria-label={`${label} ${index === 0 ? 'first' : 'second'} control point output`}
          aria-valuemin={0}
          aria-valuemax={100}
          aria-valuenow={Math.round(value[which] * 100)}
          aria-orientation="vertical"
          onPointerDown={event => {
            if (!disabled) {
              event.preventDefault()
              event.currentTarget.setPointerCapture(event.pointerId)
            }
          }}
          onPointerMove={event => moveHandle(event, which)}
          onPointerUp={event => {
            if (event.currentTarget.hasPointerCapture(event.pointerId)) {
              event.currentTarget.releasePointerCapture(event.pointerId)
            }
          }}
          onKeyDown={event => keyHandle(event, which)}
        />)}
      </svg>
      <span className="snt-curve-axis-x">Input →</span>
    </div>
    <div className="snt-curve-values">
      <span>33% input → {Math.round(value.low * 100)}%</span>
      <span>67% input → {Math.round(value.high * 100)}%</span>
      <button type="button" disabled={disabled || preset === 'linear'}
        onClick={() => onChange({ ...CURVE_PRESETS.linear })}>Reset</button>
    </div>
  </section>
}
