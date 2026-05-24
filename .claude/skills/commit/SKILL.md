---
name: commit
description: Stage changes, generate a conventional commit message, and commit. Use when ready to save progress.
disable-model-invocation: true
---

Create a git commit for the current changes in PseudoFS.

Steps:

1. Run `git status` and `git diff --stat` to see what changed.

2. If there are no changes, report "Nothing to commit" and stop.

3. Analyze the changes and draft a commit message using conventional commits format:
   - `feat: <description>` — new feature or command
   - `fix: <description>` — bug fix
   - `refactor: <description>` — code restructuring
   - `test: <description>` — adding or fixing tests
   - `docs: <description>` — documentation changes
   - `chore: <description>` — build config, tooling, etc.

   Add a scope when it's clear which module changed: `feat(unix-fs): implement ialloc/ifree`

4. Show the user:
   - Files to be committed (list them)
   - Proposed commit message
   - Ask for confirmation before committing

5. On confirmation:
   - Stage the relevant files with `git add <specific files>` (never `git add -A`)
   - Do NOT stage `.env`, credentials, `*.img`, `*.pfs`, or `build/` artifacts
   - Commit with the message
   - Show the commit hash

6. Do NOT push unless the user explicitly asks.
