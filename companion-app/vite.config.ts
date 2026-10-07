import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import electron from 'vite-plugin-electron/simple'
import { notBundle } from 'vite-plugin-electron/plugin'

export default defineConfig({
  plugins: [
    react(),

    electron({
      main: {
        entry: 'src/main/index.ts',

        // usb is a native Node module.
        // Do not bundle it into the Electron main process.
        vite: {
          plugins: [notBundle()],
        },
      },

      preload: {
        input: 'src/main/preload.ts',
      },
    }),
  ],
})