
import { usb } from 'usb'

type UsbDevice = NonNullable<
  Awaited<ReturnType<typeof usb.findDeviceByIds>>
>

export type UsbDeviceInfo = {
  vendorId: number
  productId: number
  productName?: string
  manufacturerName?: string
  serialNumber?: string
}

let activeDevice: UsbDevice | null = null
let controlInterface: number | null = null
let controlIn: number | null = null
let controlOut: number | null = null
let receiveBuffer = ''
let nextTag = 1
let tagsSupported = true

// Only allow one TILES request/response transaction at a time.
let commandQueue: Promise<void> = Promise.resolve()

export async function listUsbDevices(): Promise<UsbDeviceInfo[]> {
  return (await usb.getDevices()).map(device => ({
    vendorId: device.vendorId,
    productId: device.productId,
    productName: device.productName ?? undefined,
    manufacturerName: device.manufacturerName ?? undefined,
    serialNumber: device.serialNumber ?? undefined,
  }))
}

export async function connectUsbDevice(
  vendorId: number,
  productId: number,
) {
  await disconnectUsbDevice()

  const device = await usb.findDeviceByIds(
    vendorId,
    productId,
  )

  if (!device) {
    throw new Error(
      `USB device ${hex(vendorId)}:${hex(productId)} not found`,
    )
  }

  await device.open()

  const iface = device.configuration?.interfaces.find(
    i =>
      i.alternate.interfaceClass === 0xff &&
      i.alternate.interfaceName === 'SENTIA TILES Control',
  )

  if (!iface) {
    await device.close()
    throw new Error('SENTIA TILES Control interface not found')
  }

  const out = iface.alternate.endpoints.find(
    e => e.direction === 'out',
  )

  const input = iface.alternate.endpoints.find(
    e => e.direction === 'in',
  )

  if (!out || !input) {
    await device.close()
    throw new Error('TILES control endpoints not found')
  }

  await device.claimInterface(iface.interfaceNumber)

  activeDevice = device
  controlInterface = iface.interfaceNumber
  controlOut = out.endpointNumber
  controlIn = input.endpointNumber
  receiveBuffer = ''
  tagsSupported = true

  console.log('TILES ready:', {
    interface: controlInterface,
    outEndpoint: controlOut,
    inEndpoint: controlIn,
  })

  return {
    connected: true,
    vendorId: device.vendorId,
    productId: device.productId,
    productName: device.productName,
    manufacturerName: device.manufacturerName,
  }
}

export async function disconnectUsbDevice() {
  if (activeDevice) {
    try {
      if (controlInterface !== null) {
        await activeDevice.releaseInterface(controlInterface)
      }

      await activeDevice.close()
    } catch (error) {
      console.warn('USB disconnect error:', error)
    }
  }

  activeDevice = null
  controlInterface = null
  controlIn = null
  controlOut = null
  receiveBuffer = ''

  return { connected: false }
}

async function readLine(): Promise<string> {
  if (!activeDevice || controlIn === null) {
    throw new Error('TILES is not connected')
  }

  while (!receiveBuffer.includes('\n')) {
    const result = await activeDevice.transferIn(
      controlIn,
      64,
    )

    if (result.status !== 'ok' || !result.data) {
      throw new Error(
        `USB read failed: ${result.status}`,
      )
    }

    const data = new Uint8Array(
      result.data.buffer,
      result.data.byteOffset,
      result.data.byteLength,
    )

    receiveBuffer += new TextDecoder().decode(data)
  }

  const index = receiveBuffer.indexOf('\n')

  const line = receiveBuffer
    .slice(0, index)
    .replace(/\r$/, '')

  receiveBuffer = receiveBuffer.slice(index + 1)

  return line
}

/**
 * Public command function.
 *
 * Serializes all USB commands so multiple React components
 * cannot send overlapping requests to the instrument.
 *
 * Each command waits for the previous command to finish.
 */
export function sendTilesCommand(
  command: string,
  multiLine = false,
): Promise<string[]> {
  const result = commandQueue.then(() =>
    sendTilesCommandNow(command, multiLine),
  )

  // A failed command must not permanently block the queue.
  commandQueue = result.then(
    () => undefined,
    () => undefined,
  )

  return result
}

/**
 * Performs one complete USB request/response transaction.
 *
 * Called only from the command queue.
 */

async function sendTilesCommandNow(
  command: string,
  multiLine = false,
): Promise<string[]> {
  if (!activeDevice || controlOut === null) {
    throw new Error('TILES is not connected')
  }

  const tag = tagsSupported ? `@${nextTag} ` : ''
  nextTag = nextTag >= 99999 ? 1 : nextTag + 1

  console.log(`→ TILES: ${tag}${command}`)

  const write = await activeDevice.transferOut(
    controlOut,
    new TextEncoder().encode(`${tag}${command}\n`),
  )

  if (write.status !== 'ok') {
    throw new Error(`USB write failed: ${write.status}`)
  }

  const lines: string[] = []

  while (true) {
    const raw = await readLine()
    console.log(`← TILES: ${raw}`)

    let line = raw

    if (tag) {
      if (!raw.startsWith(tag)) {
        // Older firmware does not recognize tagged commands.
        if (raw === 'ERR unknown-command' && lines.length === 0) {
          tagsSupported = false
          return sendTilesCommandNow(command, multiLine)
        }

        // Ignore responses belonging to an earlier request.
        continue
      }

      line = raw.slice(tag.length)
    }

    if (multiLine && line === 'OK') {
      return lines
    }

    lines.push(line)

    if (!multiLine || line.startsWith('ERR ')) {
      return lines
    }
  }
}


function hex(value: number) {
  return `0x${value.toString(16).padStart(4, '0')}`
}
