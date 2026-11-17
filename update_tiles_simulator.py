#!/usr/bin/env python3
"""Update only companion-app's in-memory TILES simulator to the Alpha 6.9 settings protocol.
Run from the root of the official TILES checkout, on your sam/ branch.
Does not modify firmware, shared, or Git remotes.
"""
from pathlib import Path
import json
import subprocess
import sys

root = Path.cwd()
app = root / 'companion-app'
mock_path = app / 'src/main/mockUsb.ts'
fixture_path = app / 'fixtures/tiles-settings.json'
ui_path = app / 'src/App.tsx'
for path in [mock_path, fixture_path, ui_path]:
    if not path.is_file():
        raise SystemExit(f'Missing {path}. Run from repository root with your newer companion app checked out.')
branch = subprocess.run(['git','branch','--show-current'],capture_output=True,text=True).stdout.strip()
if not branch.startswith('sam/'):
    raise SystemExit(f'Expected a sam/ branch; currently on {branch!r}. No files were changed.')

mock = mock_path.read_text()
ui = ui_path.read_text()
fixture = json.loads(fixture_path.read_text())

def replace_once(content, old, new, label):
    if content.count(old) != 1:
        raise SystemExit(f'Unexpected {label} content (expected one match, found {content.count(old)}). No files were changed.')
    return content.replace(old,new,1)

# Keep older firmware modes intact. Alpha 6.9 uses the Alpha feature set + one new setting.
fixture['firmware'] = 'alpha.6.9'
fixture['_comment'] = ('Simulator catalog based on current official main protocol/settings table, '
    'identified as Alpha 6.9 in firmware/src/midi/product_identity.h (checked 2026-11-09). '
    'Alpha 6.9 extends the Alpha 6.5 settings with expression.pitch_bend_mode. '
    'Not tested against hardware; simulation does not emulate physical sensing.')
if 'currentSettings' not in fixture:
    fixture['currentSettings'] = []
key = 'expression.pitch_bend_mode'
entries = fixture['currentSettings']
if not any(s['key']==key for s in entries):
    entries.append({'id':515,'key':key,'type':'enum',
      'values':['legacy','cosine','path'],'default':'path','value':'path','persist':True})

mock = replace_once(mock,
    "const VERSIONS = ['0.2.0', '0.3.6', 'alpha.6.5'] as const",
    "const VERSIONS = ['0.2.0', '0.3.6', 'alpha.6.5', 'alpha.6.9'] as const",
    'mock versions')
mock = replace_once(mock, "let firmware: Version = 'alpha.6.5'", "let firmware: Version = 'alpha.6.9'", 'mock default')
old = """  const list = next === 'alpha.6.5'
    ? [...fixture.settings, ...fixture.alphaSettings]
    : next === '0.3.6'
      ? fixture.settings
      : [...fixture.settings.filter(s => !s.key.startsWith('color.')), ...fixture.legacySettings]"""
new = """  const list = next === 'alpha.6.9'
    ? [...fixture.settings, ...fixture.alphaSettings, ...fixture.currentSettings]
    : next === 'alpha.6.5'
      ? [...fixture.settings, ...fixture.alphaSettings]
      : next === '0.3.6'
        ? fixture.settings
        : [...fixture.settings.filter(s => !s.key.startsWith('color.')), ...fixture.legacySettings]"""
mock = replace_once(mock, old, new, 'version setting catalog')
mock = replace_once(mock,
    'export async function mockCommand(command: string, _multiLine = false): Promise<string[]> {',
    'async function mockCommandUntagged(command: string): Promise<string[]> {',
    'command entrypoint')
mock += '''

/** Firmware 0.3.7+ request tags. The Electron IPC API already consumes
 * the final multiline OK, so tag each returned data line, not a fabricated
 * terminator. Legacy mock profiles intentionally reject tagged requests.
 */
export async function mockCommand(command: string, _multiLine = false): Promise<string[]> {
  const trimmed = command.trim()
  if (!trimmed.startsWith('@')) return mockCommandUntagged(command)
  const match = /^@([A-Za-z0-9_-]{1,16})\\s+(.+)$/.exec(trimmed)
  if (!match) return ['ERR bad-tag']
  if (firmware === '0.2.0' || firmware === '0.3.6') return ['ERR unknown-command']
  const [, tag, cmd] = match
  const reply = await mockCommandUntagged(cmd)
  return reply.map(line => `@${tag} ${line}`)
}
'''
ui = replace_once(ui,
    '<option value="alpha.6.5">Alpha 6.5 — board-bringup</option>',
    '<option value="alpha.6.5">Alpha 6.5 — previous</option>\n              <option value="alpha.6.9">Alpha 6.9 — official main</option>',
    'simulator version dropdown')

# Write all three only after every expected region has been validated.
fixture_path.write_text(json.dumps(fixture, indent=2)+'\n')
mock_path.write_text(mock)
ui_path.write_text(ui)
print('Updated the simulator to default to Alpha 6.9, with newer pitch bend mode and tagged request support.')
print('Changed only:')
for p in [fixture_path,mock_path,ui_path]: print('  '+str(p.relative_to(root)))
print('Next: cd companion-app && npm run build; TILES_MOCK=1 npm run dev')
