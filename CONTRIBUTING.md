# Working on TILES

Who decides, how a change reaches `main`, and how people using
different AI tools stay in step. `AGENTS.md` has the short rules; this
is the long form.

## Who decides

Sentia, through Mats2000, has the final say. Every pull request into
`main` needs approval from a code owner of each path it touches
(`.github/CODEOWNERS`): Mats2000 owns everything, Sam (Samrepasky4)
co-owns `companion-app/` and `shared/mapping/`, Sebastian
(sebastianruedac) co-owns `manuals/`. Nobody can approve their own pull
request, so Mats2000 approves Sam's and Sebastian's work; as the
organization owner he can merge his own.

## A change, start to finish

1. `git switch main && git pull` — start from the latest `main`.
2. `git switch -c <you>/<topic>` — for example `sam/key-map-editor` or
   `sebastian/manual-generator`. One topic per branch.
3. Commit small and push often (`git push -u origin <branch>`). A pushed
   branch is your backup.
4. Open a pull request into `main` early (a draft is fine) and fill in
   the template: what, why, how tested, contracts, next.
5. The checks run by themselves. Fix anything red.
6. The owner reviews. Answer every comment and resolve the threads; a
   new push after an approval needs a fresh approval.
7. Once approved and green, **Squash and merge**. GitHub deletes the
   branch.

To bring the latest `main` into your branch: `git fetch` then
`git merge origin/main`. Don't rebase pushed work — it needs a
force-push, which GitHub refuses.

`firmware/board-bringup` is the one long-lived branch: Mats2000 pushes
firmware there and flashes from it, and merges it into `main` with
**Create a merge commit** after each Alpha, so `main` always has the
current firmware.

## Checks

Every pull request and every push to `main` runs
`.github/workflows/ci.yml`. `main` only accepts a pull request when the
required ones pass.

| Check | What it does |
|---|---|
| Firmware host tests | `firmware/test/run.sh` — no hardware or SDK needed. |
| Firmware build | Builds for `pico2` with Arm GNU 15.3.rel1 and Pico SDK 2.3.0 (the versions used to flash), fails on any compiler warning in `firmware/src`, and attaches the `.uf2` to the run. |
| Companion app | `npm ci` and `npm run build` in `companion-app/`. |
| Companion app lint | `npm run lint`. |

Required today: the two firmware checks. Both companion app checks
report on every pull request but don't block yet: the app on `main`
has type errors and lint errors to clear first, and each becomes
required once it is green.

When your part gets something testable (the manual generator, say), add
a job for it in the same pull request.

## Contracts

The parts only meet in `shared/`:

- `shared/protocol/` — the USB settings protocol (firmware ↔ companion
  app and `tools/`).
- `shared/mapping/` — the file a custom mapping travels in (companion app
  → manuals).
- `shared/footprint/` — TILES' face: key outlines and the pad grid, for
  every drawing.

Change a contract in its own pull request. Adding a field is fine;
renaming, removing or changing the meaning of one bumps the contract's
version, and every reader is updated in the same pull request.

## Working with AI tools

Use whichever AI you like (Claude Code, Antigravity, Codex, Cursor...).
What keeps them in step:

- **One set of rules, in the repo.** They all read `AGENTS.md` at the
  root and in the folder they work in. Antigravity reads it from
  v1.20.3; turn on *Settings → Agent → Load nested AGENTS.md files* so it
  also picks up `firmware/AGENTS.md` and `manuals/AGENTS.md`, and check
  that the rule in `.agent/rules/` shows as loaded.
- **Decisions go in the repo.** When your agent learns a rule or a
  decision is made, add it to the right AGENTS.md/README by pull request.
  An agent's private memory and your chat history are invisible to
  everyone else's tools.
- **Hand-offs happen on GitHub.** The pull request description and its
  comments carry the context; open questions and tasks are Issues. Any
  agent can read them (`gh pr view <n>`, `gh issue list`).
- **An agent only touches its person's branch and folder.** It never
  pushes to someone else's branch, never merges, and never changes repo
  settings.
- **You are the author.** Read what your agent changed before you push.

## Releases and tags

- Firmware versions are Alpha N.P: a small change is the next point
  (6.4 → 6.5), a new Alpha is for big changes (architecture, features,
  layouts). Mats2000 tags the flashed commit `firmware/alpha-N.P`.
- `archive/...` tags keep old work that isn't on any branch.
- Tags can't be moved or deleted.

## What GitHub enforces

- `main`: changes only through a pull request with a code owner's
  approval, resolved conversations and passing checks; no force-push; no
  deletion.
- `firmware/board-bringup`: no force-push, no deletion.
- Every branch: no force-push.
- Tags: no moving or deleting.

Mats2000, as organization owner, can step past these when something
needs fixing; nobody else can.

## Access

- Sign in to GitHub on your own machine (`gh auth login`, or your
  editor's GitHub sign-in). Never share a password or token with anyone,
  person or AI.
- Turn on two-factor authentication for your GitHub account.
