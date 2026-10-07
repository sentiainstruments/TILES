import {app, BrowserWindow, ipcMain,} from 'electron'
import { dirname, join,} from 'node:path'
import {  fileURLToPath,} from 'node:url'
import { usb } from 'usb'
import { connectUsbDevice, disconnectUsbDevice, listUsbDevices, sendTilesCommand,} from './usb'


const __dirname = dirname(
  fileURLToPath(import.meta.url),
)

let mainWindow: BrowserWindow | null = null

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1200,
    height: 800,

    webPreferences: {
      preload: join(
        __dirname,
        'preload.mjs',
      ),

      contextIsolation: true,
      nodeIntegration: false,
    },
  })

  if (process.env.VITE_DEV_SERVER_URL) {
    mainWindow.loadURL(
      process.env.VITE_DEV_SERVER_URL,
    )
  } else {
    mainWindow.loadFile(
      'dist/index.html',
    )
  }
}

async function sendUsbList() {
  const devices =
    await listUsbDevices()

  mainWindow?.webContents.send(
    'usb:changed',
    devices,
  )
}

app.whenReady().then(() => {
  ipcMain.handle(
    'usb:list',
    async () => {
      return listUsbDevices()
    },
  )

  ipcMain.handle(
    'usb:connect',
    async (
      _event,
      vendorId: number,
      productId: number,
    ) => {
      return connectUsbDevice(
        vendorId,
        productId,
      )
    },
  )

  ipcMain.handle(
    'usb:disconnect',
    async () => {
      return disconnectUsbDevice()
    },
  )

  // usb v3 / WebUSB events
  usb.addEventListener(
    'connect',
    () => {
      void sendUsbList()
    },
  )

  usb.addEventListener(
    'disconnect',
    () => {
      void sendUsbList()
    },
  )
  ipcMain.handle(
  'usb:command',
  async (
    _event,
    command: string,
    multiLine = false,
  ) => {
    return sendTilesCommand(
      command,
      multiLine,
    )
  },
)

  createWindow()
})

app.on(
  'window-all-closed',
  () => {
    if (
      process.platform !== 'darwin'
    ) {
      app.quit()
    }
  },
)

app.on(
  'activate',
  () => {
    if (
      BrowserWindow
        .getAllWindows()
        .length === 0
    ) {
      createWindow()
    }
  },
)