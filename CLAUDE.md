# AdaptTree — Claude Instructions

## Project

AdaptTree is a C++20 B+ tree storage engine with WAL, MVCC, differential fuzzing, and a PGM-style learned index layer. See `.planning/PROJECT.md` for full context.

## Hard Rules

- **Never add Claude as a co-author or contributor** in git commits, PR descriptions, README, or any project file. Commits are authored by the developer only.
- **Never upload Claude-generated support files to GitHub.** This includes `.planning/`, `docs/superpowers/`, and any AI scaffold files. Add these to `.gitignore`.

## Git

- Commit messages are written in imperative mood (`feat:`, `test:`, `fix:`, `bench:`, `docs:`).
- No `Co-Authored-By: Claude` or similar lines in commit messages.
- Commits must be atomic — one logical change per commit.

## Code Style

- C++20, `#pragma once`, no C++20 named modules.
- `CMAKE_CXX_EXTENSIONS OFF`.
- No external ML or learned-index libraries — PGM construction is pure geometry.
- `pread`/`pwrite` for all file I/O (POSIX only; WSL2 acceptable for dev).

## Phase Summaries

After every phase is complete (all tests green, final commit done), write a detailed summary to `.planning/phases/phase-N/SUMMARY.md`. The summary must include:

1. **Status and test count** — e.g. "36/36 green"
2. **Requirements covered** — list each requirement ID
3. **What was built** — one paragraph describing the purpose of the phase
4. **Files created** — for every file: its path, what it contains, why each design decision was made, and how it connects to other layers
5. **Page/data layout** — ASCII diagram if the phase introduces any in-memory or on-disk layout
6. **Key design decisions** — numbered list of non-obvious choices and the reasoning
7. **Commits** — list of commit messages in order
8. **Interface contract for the next phase** — what the next phase may call and what invariants it must preserve

Summaries live in `.planning/` so they are gitignored and never pushed to GitHub.

## What NOT to do

- Do not refactor code outside the current task scope.
- Do not add docstrings or comments to code you didn't change.
- Do not add error handling for scenarios that can't happen.
- Do not design for hypothetical future requirements.
