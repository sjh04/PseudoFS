---
name: test-fs
description: Smoke-test the file system by running a sequence of commands against the built PseudoFS executable. Use after building to verify basic functionality.
---

Run a quick smoke test on PseudoFS:

1. First, ensure the project is built (run `/build` if needed).
2. Remove any existing test disk image: `rm -f build/test.img`
3. Run the executable with `--format` to create a fresh virtual disk.
4. Feed a sequence of commands to exercise core functionality:
   - `format` (if not auto-formatted)
   - `login` as root
   - `mkdir testdir`
   - `cd testdir`
   - `touch hello.txt`
   - `write hello.txt` with sample content
   - `read hello.txt` and verify output
   - `ls` and verify hello.txt appears
   - `cd ..`
   - `rm -r testdir`
   - `ls` and verify testdir is gone
   - `logout`

5. Report which commands succeeded and which failed.
6. If any command fails, read the relevant source code and suggest a fix.

Note: The exact command interface depends on the current implementation state. Adapt the test sequence to whatever commands are available. The goal is to exercise as much of the FS pipeline as possible.
