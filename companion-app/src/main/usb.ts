
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
  if (drumUploadActive) throw new Error('Sample upload in progress')
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

  try {
    await device.claimInterface(iface.interfaceNumber)
  } catch (error) {
    try {
      await device.close()
    } catch (closeError) {
      console.warn('USB cleanup failed:', closeError)
    }

    throw error
  }

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
  if (drumUploadActive) throw new Error('Sample upload in progress')
  const device = activeDevice
  const iface = controlInterface

  activeDevice = null
  controlInterface = null
  controlIn = null
  controlOut = null
  receiveBuffer = ''

  if (device) {
    try {
      if (iface !== null) {
        await device.releaseInterface(iface)
      }
    } catch (error) {
      console.warn('USB release error:', error)
    }

    try {
      await device.close()
    } catch (error) {
      console.warn('USB close error:', error)
    }
  }

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

// TILES drum samples: one exclusive command+binary+verdict transaction.
let drumUploadActive = false
export function uploadDrumSample(input: unknown): Promise<void> {
  if (!input || typeof input !== 'object') return Promise.reject(new Error('Invalid sample'))
  const {slot,name,channels,frames,bytes} = input as {slot:number;name:string;channels:number;frames:number;bytes:number[]}
  if (!Number.isInteger(slot) || slot<1 || slot>8 || typeof name!=='string' || !/^[A-Za-z0-9_-]{1,23}$/.test(name) ||
      (channels!==1 && channels!==2) || !Number.isInteger(frames) || frames<1 || frames>(channels===1?96000:48000) ||
      !Array.isArray(bytes) || bytes.length!==frames*channels*2 || !bytes.every(b=>Number.isInteger(b)&&b>=0&&b<=255)) {
    return Promise.reject(new Error('Invalid sample format or length'))
  }
  const data=Uint8Array.from(bytes)
  let crc=0xffffffff
  for(const b of data){crc^=b;for(let j=0;j<8;j++)crc=(crc>>>1)^((crc&1)?0xedb88320:0)}
  const checksum=((crc^0xffffffff)>>>0).toString(16).padStart(8,'0')
  const result=commandQueue.then(async()=>{
    if(!activeDevice || controlOut===null)throw new Error('TILES is not connected')
    const device=activeDevice, endpoint=controlOut
    const anticipatedTag=tagsSupported?`@${nextTag} `:''
    drumUploadActive=true
    let uncertain=true
    try {
      const [reply]=await sendTilesCommandNow(`SAMPLE BEGIN ${slot} ${channels} ${frames} ${checksum} ${name}`,false)
      if(reply?.startsWith('ERR ')){uncertain=false;throw new Error(reply==='ERR playing'?'Stop playback and wait for the instrument to become silent.':reply)}
      if(reply!==`OK ${data.length}`)throw new Error(`Unexpected sample handshake: ${reply}`)
      const tag=tagsSupported?anticipatedTag:''
      for(let i=0;i<data.length;i+=4096){
        const chunk=data.slice(i,i+4096)
        const write=await device.transferOut(endpoint,chunk)
        if(write.status!=='ok'||write.bytesWritten!==chunk.length)throw new Error('Incomplete sample write')
      }
      while(true){
        const raw=await readLine()
        if(tag&&!raw.startsWith(tag))continue
        const verdict=tag?raw.slice(tag.length):raw
        if(verdict==='OK'){uncertain=false;return}
        if(verdict.startsWith('ERR ')){uncertain=false;throw new Error(verdict)}
        throw new Error(`Unexpected sample verdict: ${verdict}`)
      }
    }catch(error){
      if(uncertain){
        // Don't let queued text commands become sample bytes. Wait beyond the
        // firmware's 3 s raw timeout, then close the connection before releasing the queue.
        await new Promise<void>(resolve=>setTimeout(resolve,3500))
        drumUploadActive=false
        await disconnectUsbDevice()
        throw new Error(`${error instanceof Error?error.message:String(error)}. Reconnect TILES and check the slot; it may be empty.`)
      }
      throw error
    }finally{drumUploadActive=false}
  })
  commandQueue=result.then(()=>undefined,()=>undefined)
  return result
}
