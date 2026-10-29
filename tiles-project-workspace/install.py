#!/usr/bin/env python3
"""Install TILES project workspace without modifying main or silently dropping local edits."""
from pathlib import Path
import argparse
import shutil
import subprocess
import json
import sys

ROOT = Path(__file__).resolve().parent
p = argparse.ArgumentParser(description='Install project workspace into an existing TILES checkout.')
p.add_argument('repository', nargs='?', default='.', help='Root of your TILES checkout')
p.add_argument('--dry-run', action='store_true')
a = p.parse_args()
repo = Path(a.repository).resolve()
app = repo / 'companion-app'
if not (app/'src/main/index.ts').is_file() or not (app/'src/App.tsx').is_file():
    sys.exit('ERROR: Run this with the TILES repository root as the argument.')
fixture = app / 'fixtures/tiles-settings.json'
if not fixture.is_file() or 'alphaSettings' not in json.loads(fixture.read_text()):
    sys.exit('ERROR: Update sam-branch to the Alpha 6.5 simulator first (fixture missing alphaSettings).')

changes: dict[Path,str] = {}
new_files = [
    'src/project/model.ts', 'src/project/catalog.ts', 'src/project/PresetVisualizer.tsx',
    'src/project/deviceApply.ts', 'src/project/ProjectWorkspace.tsx',
    'src/project/ProjectWorkspace.css', 'src/main/projectFiles.ts',
]
for item in new_files:
    dst = app / item
    src = ROOT / 'companion-app' / item
    if dst.exists() and dst.read_text() != src.read_text():
        sys.exit(f'ERROR: Existing {item} differs. Stop to inspect before overwriting it.')
    changes[dst] = src.read_text()

def replace_once(s: str, old: str, new: str, file: Path) -> str:
    if new in s:
        return s
    if s.count(old) != 1:
        sys.exit(f'ERROR: Could not safely patch {file.name}: expected one matching insertion point.')
    return s.replace(old, new, 1)

main = app/'src/main/index.ts'
s = main.read_text()
s = replace_once(s, "import { mockCommand, mockDevice } from './mockUsb'",
    "import { mockCommand, mockDevice } from './mockUsb'\nimport { registerProjectIpc } from './projectFiles'", main)
s = replace_once(s, 'app.whenReady().then(() => {',
    'app.whenReady().then(() => {\n  registerProjectIpc(() => mainWindow)', main)
changes[main] = s

preload = app/'src/main/preload.ts'
s = preload.read_text()
s = replace_once(s, "    usb: {", '''    project: {
      open: () => ipcRenderer.invoke('project:open'),
      save: (project: unknown, saveAs = false) =>
        ipcRenderer.invoke('project:save', project, saveAs),
      reset: () => ipcRenderer.invoke('project:new'),
    },
    usb: {''', preload)
changes[preload] = s

decl = app/'src/electron.d.ts'
s = decl.read_text()
s = replace_once(s, "    tiles: {\n      usb: {", '''    tiles: {
      project: {
        open(): Promise<{
          project: import('./project/model').TilesProject
          fileName: string
        } | null>
        save(project: import('./project/model').TilesProject, saveAs?: boolean):
          Promise<{ fileName: string } | null>
        reset(): Promise<boolean>
      }
      usb: {''', decl)
changes[decl] = s

original_app = app/'src/App.tsx'
s = original_app.read_text()
if 'ProjectWorkspace' not in s:
    # This overlay preserves the pre-existing settings pages. If a user has
    # independently added another page, refuse to replace it automatically.
    known_blob_sha = 'acad8447c79053603097673c2d984bd24e170c7d'
    try:
        sha = subprocess.check_output(['git', 'hash-object', str(original_app)], cwd=repo, text=True).strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        sys.exit('ERROR: Git is needed to verify App.tsx before replacing it.')
    if sha != known_blob_sha:
        sys.exit('ERROR: Your App.tsx has custom changes. Nothing was changed; merge the supplied App.tsx with yours manually to preserve them.')
    changes[original_app] = (ROOT/'companion-app/src/App.tsx').read_text()

print('Files to add/update:')
for file, content in changes.items():
    label = 'NEW' if not file.exists() else ('SAME' if file.read_text() == content else 'EDIT')
    print(f'  {label:4}  {file.relative_to(repo)}')
if a.dry_run:
    print('Dry run: no files written.')
    sys.exit(0)

# Copy backups first so applying a patch never destroys the original.
for file, content in changes.items():
    file.parent.mkdir(parents=True, exist_ok=True)
    if file.exists() and file.read_text() != content:
        backup = file.with_suffix(file.suffix + '.before-project-workspace.bak')
        if backup.exists():
            sys.exit(f'ERROR: Backup already exists at {backup}; remove only after inspecting it.')
        shutil.copy2(file, backup)
for file, content in changes.items():
    file.write_text(content)
print('\nInstalled. Originals are backed up as *.before-project-workspace.bak.')
print('Next: cd companion-app && npm run build && TILES_MOCK=1 npm run dev')
