type WorkspaceKind = 'project' | 'device' | 'synth' | 'preview' | 'edit' | 'refresh' | 'speaker' | 'reset'

const labels: Record<WorkspaceKind, string> = {
  project: 'Project Workspace',
  device: 'Device Settings',
  synth: 'Synth Workspace',
  preview: 'Preview Mode',
  edit: 'Edit Preset',
  refresh: 'Refresh',
  speaker: 'Audition current sound',
  reset: 'Restore factory preset',
}

export function WorkspaceIcon({ kind }: { kind: WorkspaceKind }) {
  return <span className="snt-workspace-icon" data-workspace={kind}>
    <svg viewBox="0 0 24 24" width="24" height="24" fill="none" stroke="currentColor"
      strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true" focusable="false">
      {kind === 'project' && <>
        <path d="M3 21V11l6-4v5l6-5v5h6v9H3Z" />
        <path d="M17 12V3h3v9M7 16h1m3 0h1m3 0h1" />
      </>}
      {kind === 'device' && <path d="M14.5 6.5a5 5 0 0 1 6-3.2l-3.4 3.4 2.2 2.2 3.4-3.4a5 5 0 0 1-6.2 6.2L7 21.2a2.6 2.6 0 0 1-3.7-3.7l9.5-9.5a5 5 0 0 1 1.7-1.5Z" />}
      {kind === 'synth' && <>
        <rect x="2.5" y="4.5" width="19" height="15" rx="2" />
        <path d="M7.2 13v6.5M12 13v6.5M16.8 13v6.5" />
        <path d="M6 4.5h2.4V13H6zM10.8 4.5h2.4V13h-2.4zM15.6 4.5H18V13h-2.4z" fill="currentColor" stroke="none" />
      </>}
      {kind === 'preview' && <>
        <path d="M2 12s3.6-7 10-7 10 7 10 7-3.6 7-10 7S2 12 2 12Z" />
        <circle cx="12" cy="12" r="3" />
      </>}
      {kind === 'edit' && <>
        <path d="m15 5 4 4" />
        <path d="M4 20l1-5L16.5 3.5a2.8 2.8 0 0 1 4 4L9 19l-5 1Z" />
      </>}
      {kind === 'refresh' && <>
        <path d="M20 11a8 8 0 1 0-2.3 6.7" />
        <path d="M20 4v7h-7" />
      </>}
      {kind === 'speaker' && <>
        <path d="M11 5 6 9H3v6h3l5 4V5Z" />
        <path d="M15 9a4 4 0 0 1 0 6m3-9a8 8 0 0 1 0 12" />
      </>}
      {kind === 'reset' && <>
        <path d="M3 12a9 9 0 1 1 2.6 6.4M3 12V5m0 7h7" />
      </>}
    </svg>
    <span className="snt-workspace-sr-label">{labels[kind]}</span>
  </span>
}
