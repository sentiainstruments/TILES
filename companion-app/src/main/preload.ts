import { contextBridge, ipcRenderer,} from 'electron'

contextBridge.exposeInMainWorld(
  'tiles',
  {
    usb: {
      list: () =>
        ipcRenderer.invoke('usb:list'),

      connect: (
        vendorId: number,
        productId: number,
      ) =>
        ipcRenderer.invoke(
          'usb:connect',
          vendorId,
          productId,
        ),

      disconnect: () =>
        ipcRenderer.invoke(
          'usb:disconnect',
        ),

      onChanged: (
        callback: (devices: unknown[]) => void,
      ) => {
        const listener = (
          _event: Electron.IpcRendererEvent,
          devices: unknown[],
        ) => {
          callback(devices)
        }

        ipcRenderer.on(
          'usb:changed',
          listener,
        )

        return () => {
          ipcRenderer.removeListener(
            'usb:changed',
            listener,
          )
        }
      },
      command: (
        command: string,
        multiLine = false,
        ) =>
        ipcRenderer.invoke(
            'usb:command',
            command,
            multiLine,
        ),
    },
  },
)