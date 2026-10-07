export async function getSetting(key: string) {
  const [value] = await window.tiles.usb.command(`GET ${key}`)
  if (value.startsWith('ERR ')) throw new Error(value)
  return value
}

export async function setSetting(
  key: string,
  value: string | number | boolean,
) {
  const [response] = await window.tiles.usb.command(`SET ${key} ${value}`)
  if (response !== 'OK') throw new Error(response)
}

export async function listSettings() {
  return window.tiles.usb.command('LIST', true)
}