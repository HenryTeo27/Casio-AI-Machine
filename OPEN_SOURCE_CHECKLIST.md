# Open Source Release Checklist

Use this checklist before making the repository public.

## 1. Secrets and Privacy

- [ ] Confirm `.env.local` is never committed.
- [ ] Confirm no API keys are present in tracked files.
- [ ] Confirm no production DB URLs are present in tracked files.
- [ ] Confirm no private hostnames/domains are present in docs.
- [ ] Rotate any key that was ever exposed during development.

## 2. Repository Basics

- [ ] `README.md` explains project purpose and architecture.
- [ ] `LICENSE` exists (MIT).
- [ ] `STEP_BY_STEP.md` exists and is up to date.
- [ ] `CONTRIBUTING.md` exists.
- [ ] `SECURITY.md` exists.

## 3. Code and Build

- [ ] Firmware builds on your machine using PlatformIO.
- [ ] Main demo/test paths are documented.
- [ ] Pin mappings and hardware assumptions are documented.
- [ ] Backend copy in `server/` is clearly marked as template/reference.

## 4. Docs Quality

- [ ] Broken links check passed in markdown files.
- [ ] Photos in `assets/` render correctly in `README.md`.
- [ ] Setup docs use placeholder values, not private values.
- [ ] Prompt/harness docs match the current workflow.

## 5. Release Hygiene

- [ ] Commit history does not contain obvious secret leaks.
- [ ] Add repository topics and a short description on GitHub.
- [ ] Add at least one release tag (for example `v0.1.0`).
- [ ] Optionally pin key docs in repository homepage.

## 6. Optional But Recommended

- [ ] Add a short demo video/GIF in `README.md`.
- [ ] Add hardware safety note (battery, soldering, grinding).
- [ ] Add a disclaimer for responsible use.
