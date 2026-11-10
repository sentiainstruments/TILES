import { useEffect, useState } from 'react'

export function useTilesSettings(keys: Record<string, string>) {
  const [values, setValues] = useState<Record<string, string>>({})
  const [error, setError] = useState('')

  // Keys are often constructed inline by pages. Depend on their contents,
  // not object identity, to avoid re-fetch loops on every state update.
  const keySignature = JSON.stringify(keys)

  useEffect(() => {
    let cancelled = false
    const requestedKeys = JSON.parse(keySignature) as Record<string, string>
    void Promise.all(Object.entries(requestedKeys).map(async ([name, key]) => {
      try {
        const [value] = await window.tiles.usb.command(`GET ${key}`)
        if (cancelled) return
        if (!value || value.startsWith('ERR ')) {
          setError(`${key}: ${value ?? 'no response'}`)
          return
        }
        setValues(v => ({ ...v, [name]: value }))
      } catch (reason) {
        if (!cancelled) setError(`${key}: ${String(reason)}`)
      }
    }))
    return () => { cancelled = true }
  }, [keySignature])

  async function update(name: string, value: string) {
    const key = keys[name]
    if (!key) return
    try {
      const [response] = await window.tiles.usb.command(`SET ${key} ${value}`)
      if (response !== 'OK') {
        setError(`${key}: ${response}`)
        return
      }
      setValues(prev => ({ ...prev, [name]: value }))
      window.dispatchEvent(new CustomEvent('tiles:setting-changed', {
        detail: { key, value },
      }))
      setError('')
    } catch (reason) {
      setError(`${key}: ${String(reason)}`)
    }
  }
  return { values, update, error }
}
