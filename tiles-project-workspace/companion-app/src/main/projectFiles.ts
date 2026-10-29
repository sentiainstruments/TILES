import { dialog, ipcMain, type BrowserWindow } from 'electron'
import { readFile, rename, writeFile } from 'node:fs/promises'
import { basename, dirname, join } from 'node:path'
import { randomUUID } from 'node:crypto'
import { serializeProject, validateProject, type TilesProject } from '../project/model'

const MAX_BYTES = 2 * 1024 * 1024
let currentFile: string | null = null

export function registerProjectIpc(getWindow: () => BrowserWindow | null) {
  ipcMain.handle('project:open', async () => {
    const parent = getWindow()
    const options = {
      title: 'Open TILES Project',
      properties: ['openFile'] as Array<'openFile'>,
      filters: [{ name: 'TILES Project', extensions: ['tilesproject'] }],
    }
    const choice = parent
      ? await dialog.showOpenDialog(parent, options)
      : await dialog.showOpenDialog(options)
    const path = choice.filePaths[0]
    if (choice.canceled || !path) return null
    if (!path.toLowerCase().endsWith('.tilesproject')) throw new Error('Choose a .tilesproject file.')
    const raw = await readFile(path, 'utf8')
    if (Buffer.byteLength(raw, 'utf8') > MAX_BYTES) throw new Error('Project exceeds 2 MB.')
    const project = validateProject(JSON.parse(raw))
    currentFile = path
    return { project, fileName: basename(path) }
  })

  ipcMain.handle('project:save', async (_event, value: unknown, saveAs: boolean) => {
    const project: TilesProject = validateProject(value)
    const raw = serializeProject(project)
    if (Buffer.byteLength(raw, 'utf8') > MAX_BYTES) throw new Error('Project exceeds 2 MB.')
    let path = saveAs ? null : currentFile
    if (!path) {
      const parent = getWindow()
      const options = {
        title: 'Save TILES Project',
        defaultPath: `${project.name.replace(/[^a-zA-Z0-9 _-]/g, '').trim() || 'Untitled'}.tilesproject`,
        filters: [{ name: 'TILES Project', extensions: ['tilesproject'] }],
      }
      const choice = parent
        ? await dialog.showSaveDialog(parent, options)
        : await dialog.showSaveDialog(options)
      if (choice.canceled || !choice.filePath) return null
      path = choice.filePath.toLowerCase().endsWith('.tilesproject')
        ? choice.filePath : `${choice.filePath}.tilesproject`
    }
    // Atomic replace protects the previous file if the application stops mid-save.
    const temporary = join(dirname(path), `.${basename(path)}.${randomUUID()}.tmp`)
    try {
      await writeFile(temporary, raw, { encoding: 'utf8', flag: 'wx' })
      await rename(temporary, path)
    } catch (error) {
      const { unlink } = await import('node:fs/promises')
      await unlink(temporary).catch(() => undefined)
      throw error
    }
    currentFile = path
    return { fileName: basename(path) }
  })

  ipcMain.handle('project:new', () => {
    currentFile = null
    return true
  })
}
