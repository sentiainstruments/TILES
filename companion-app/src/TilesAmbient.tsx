import { useEffect, useRef } from 'react'

const TAU = Math.PI * 2
const clamp = (n: number) => Math.max(0, Math.min(1, n))
const wave = (phase: number) => 0.5 + 0.5 * Math.sin(TAU * phase)
const mod = (n: number, d: number) => ((n % d) + d) % d
const SCENE_MS = 24000
const SCENES = ['wave', 'glow', 'stars', 'rgb'] as const
type Scene = typeof SCENES[number]
type Light = { level: number; hue: number | null }

// Ambient adaptations of the equations in firmware/src/services/standby.c.
// Rows 0–4, columns 1–6 (row 0 is the device's button row).
// This is a purely decorative field, not a device state or LED simulation.
function lightAt(scene: Scene, row: number, col: number, ms: number): Light {
  const diag = row + col
  if (scene === 'wave') return { level: wave(diag / 3 - ms / 3000) ** 2.2, hue: null }
  if (scene === 'glow') {
    const distance = Math.hypot(row - 2, col - 3.5)
    const ring = mod(ms / 2500 * 4.2, 4.2)
    return { level: Math.exp(-((distance - ring) ** 2) / (0.5 ** 2)), hue: null }
  }
  if (scene === 'rgb') return {
    level: row === 0 ? 0.35 : wave(diag / 3 - ms / 3500) ** 1.8,
    hue: row === 0 ? null : 220 + 65 * wave(ms / 6000 + diag * 0.04),
  }
  // Five deterministic falling comets; analogous to the firmware's randomly respawning stars.
  let level = 0
  for (let i = 0; i < 5; i++) {
    const tail = 2 + (i * 0.61) % 2.5
    const speed = 1 / (600 - i * 70)
    const duration = (4 + 2 * tail) / speed
    const age = ms + i * 700
    const cycle = Math.floor(age / duration)
    const column = 1 + mod(i * 5 + cycle * 3 + Math.floor(cycle / 3), 6)
    if (column !== col) continue
    const head = mod(age, duration) * speed - tail
    const behind = head - row
    if (behind >= 0 && behind <= tail) {
      const twinkle = 1 - 0.25 * (0.5 + 0.5 * Math.sin(ms / 90 + i * 1.7))
      level = Math.max(level, Math.exp(-2.2 * behind / tail) * twinkle)
    }
  }
  return { level: clamp(level), hue: null }
}

export function TilesAmbient() {
  const ref = useRef<HTMLCanvasElement>(null)
  useEffect(() => {
    const canvas = ref.current
    if (!canvas) return
    const ctx = canvas.getContext('2d')
    if (!ctx) return
    const motion = window.matchMedia('(prefers-reduced-motion: reduce)')
    let frame = 0
    let last = -Infinity
    let elapsed = 0
    let previous: number | null = null
    let visible = true
    let w = 0, h = 0
    const resize = () => {
      // Low-resolution light field is intentionally soft; avoid a large GPU surface.
      const rect = canvas.getBoundingClientRect()
      w = Math.max(1, Math.round(rect.width * 0.5))
      h = Math.max(1, Math.round(rect.height * 0.5))
      canvas.width = w
      canvas.height = h
      last = -Infinity
    }
    const observer = new ResizeObserver(resize)
    observer.observe(canvas)
    resize()
    function draw(ms: number) {
      if (!ctx) return
      ctx.clearRect(0, 0, w, h)
      const scene = SCENES[Math.floor(ms / SCENE_MS) % SCENES.length]
      const age = mod(ms, SCENE_MS)
      const fade = Math.min(1, age / 1400, (SCENE_MS - age) / 1400)
      const radius = Math.max(w / 6, h / 5) * 1.10
      for (let row = 0; row < 5; row++) for (let col = 1; col <= 6; col++) {
        const { level, hue } = lightAt(scene, row, col, age * 0.72)
        const alpha = level * fade * 0.75
        if (alpha < 0.012) continue
        const x = (col - 0.5) * w / 6, y = (row + 0.5) * h / 5
        const color = hue === null ? '207, 224, 255' : null
        const gradient = ctx.createRadialGradient(x, y, 0, x, y, radius)
        gradient.addColorStop(0, color ? `rgba(${color}, ${alpha})` : `hsla(${hue}, 92%, 65%, ${alpha})`)
        gradient.addColorStop(1, color ? `rgba(${color}, 0)` : `hsla(${hue}, 92%, 65%, 0)`)
        ctx.fillStyle = gradient
        ctx.fillRect(x - radius, y - radius, radius * 2, radius * 2)
      }
    }
    function tick(now: number) {
      frame = 0
      if (!visible || document.hidden || motion.matches) { previous = null; return }
      if (previous !== null) elapsed += Math.min(now - previous, 100)
      previous = now
      if (now - last >= 40) { draw(elapsed + 1400); last = now }
      frame = requestAnimationFrame(tick)
    }
    function sync() {
      if (frame) cancelAnimationFrame(frame)
      frame = 0
      previous = null
      if (motion.matches) { ctx?.clearRect(0, 0, w, h); return }
      if (visible && !document.hidden) frame = requestAnimationFrame(tick)
    }
    const intersection = new IntersectionObserver(entries => { visible = entries[0]?.isIntersecting ?? false; sync() })
    intersection.observe(canvas)
    document.addEventListener('visibilitychange', sync)
    motion.addEventListener('change', sync)
    sync()
    return () => {
      if (frame) cancelAnimationFrame(frame)
      observer.disconnect()
      intersection.disconnect()
      document.removeEventListener('visibilitychange', sync)
      motion.removeEventListener('change', sync)
    }
  }, [])
  return <canvas ref={ref} className="snt-tiles-ambient" aria-hidden="true" />
}
