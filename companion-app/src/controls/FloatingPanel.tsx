import { useEffect, useId, useLayoutEffect, useRef, useState } from 'react'
import type { PointerEvent, ReactNode } from 'react'
import { createPortal } from 'react-dom'
import './FloatingPanel.css'

type Position = { x: number; y: number }
const MARGIN = 12

function clampPosition(position: Position, width: number, height: number): Position {
  return {
    x: Math.max(MARGIN, Math.min(position.x, window.innerWidth - width - MARGIN)),
    y: Math.max(MARGIN, Math.min(position.y, window.innerHeight - height - MARGIN)),
  }
}

/** Non-modal, in-window floating tool panel. Its children share the existing React tree. */
export function FloatingPanel({ title, children, onClose }: {
  title: string
  children: ReactNode
  onClose: () => void
}) {
  const titleId = useId()
  const panel = useRef<HTMLDivElement>(null)
  const closeButton = useRef<HTMLButtonElement>(null)
  const drag = useRef<{ id: number; dx: number; dy: number } | null>(null)
  const [position, setPosition] = useState<Position>({ x: MARGIN, y: MARGIN })

  useLayoutEffect(() => {
    const element = panel.current
    if (!element) return
    const rect = element.getBoundingClientRect()
    setPosition(clampPosition({ x: (window.innerWidth - rect.width) / 2,
      y: (window.innerHeight - rect.height) / 2 }, rect.width, rect.height))
    const constrain = () => {
      const bounds = element.getBoundingClientRect()
      setPosition(old => {
        const next = clampPosition(old, bounds.width, bounds.height)
        return next.x === old.x && next.y === old.y ? old : next
      })
    }
    const observer = new ResizeObserver(constrain)
    observer.observe(element)
    window.addEventListener('resize', constrain)
    return () => {
      observer.disconnect()
      window.removeEventListener('resize', constrain)
    }
  }, [])

  useEffect(() => {
    const previous = document.activeElement
    closeButton.current?.focus({ preventScroll: true })
    return () => {
      // Do not steal focus from the underlying workspace if the user moved there.
      const active = document.activeElement
      if (previous instanceof HTMLElement && previous.isConnected &&
          (active === document.body || (active && panel.current?.contains(active)))) {
        previous.focus({ preventScroll: true })
      }
    }
  }, [])

  function startDrag(event: PointerEvent<HTMLDivElement>) {
    if (event.button !== 0 || !event.isPrimary || drag.current || !panel.current) return
    const target = event.target
    if (target instanceof Element && target.closest('button')) return
    const rect = panel.current.getBoundingClientRect()
    drag.current = { id: event.pointerId, dx: event.clientX - rect.left, dy: event.clientY - rect.top }
    event.currentTarget.setPointerCapture(event.pointerId)
    event.preventDefault()
  }

  function moveDrag(event: PointerEvent<HTMLDivElement>) {
    const current = drag.current
    if (!current || current.id !== event.pointerId || !panel.current) return
    const rect = panel.current.getBoundingClientRect()
    setPosition(clampPosition({ x: event.clientX - current.dx, y: event.clientY - current.dy }, rect.width, rect.height))
  }

  function endDrag(event: PointerEvent<HTMLDivElement>) {
    if (drag.current?.id !== event.pointerId) return
    drag.current = null
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId)
    }
  }

  return createPortal(
    <div ref={panel} className="snt-floating-panel" role="dialog" aria-modal={false}
      aria-labelledby={titleId} style={{ left: position.x, top: position.y }}
      onKeyDown={event => {
        if (event.key === 'Escape' && !event.defaultPrevented) {
          event.stopPropagation()
          onClose()
        }
      }}>
      <div className="snt-floating-titlebar" onPointerDown={startDrag} onPointerMove={moveDrag}
        onPointerUp={endDrag} onPointerCancel={endDrag} onLostPointerCapture={endDrag}>
        <span id={titleId}>{title}</span>
        <button type="button" ref={closeButton} className="snt-floating-close" aria-label={`Close ${title}`}
          onClick={onClose}>×</button>
      </div>
      <div className="snt-floating-content">{children}</div>
    </div>, document.body)
}
