export {}

type UsbDeviceInfo = {
  vendorId: number
  productId: number
  deviceClass: number
  busNumber: number
  deviceAddress: number
}

declare global {
  interface Window {
    tiles: {
      project: {
        open(): Promise<{
          project: import('./project/model').TilesProject
          fileName: string
        } | null>
        save(project: import('./project/model').TilesProject, saveAs?: boolean):
          Promise<{ fileName: string } | null>
        reset(): Promise<boolean>
      }
      usb: {
        list(): Promise<UsbDeviceInfo[]>

        connect(
          vendorId: number,
          productId: number,
        ): Promise<{
          connected: boolean
          vendorId: number
          productId: number
          interfaceNumber: number
          endpoints: {
            address: number
            direction: string
            transferType: number
          }[]
        }>

        disconnect(): Promise<{
          connected: boolean
        }>
        command(
            command: string,
            multiLine?: boolean,
        ): Promise<string[]>

        onChanged(
          callback: (
            devices: UsbDeviceInfo[],
          ) => void,
        ): () => void
      }
    }
  }
}