import { useEffect, useState } from 'react'

export function useTilesSettings(keys: Record<string, string>) {
  const [values, setValues] = useState<Record<string, string>>({})
  const [error, setError] = useState('')

  useEffect(() => {
    Object.entries(keys).forEach(async ([name, key]) => {
      const [value] = await window.tiles.usb.command(`GET ${key}`)
      if (value.startsWith('ERR ')) return setError(`${key}: ${value}`)
      setValues(v => ({ ...v, [name]: value }))
    })
  }, [])

  async function update(name: string, value: string) {
    const [response] = await window.tiles.usb.command(`SET ${keys[name]} ${value}`)
    if (response !== 'OK') return setError(`${keys[name]}: ${response}`)
    setValues(v => ({ ...v, [name]: value }))
    setError('')
  }

  return { values, update, error }
}