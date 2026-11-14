---
name: upstream-port
description: "Find a fix that zortos293/Lichii (our upstream) or Eglantine (Lichii's upstream) already has and port it with credit: search pull requests, issues and branches by game and symptom, read the diffs, cherry-pick with -x or re-apply with Co-authored-by, cite it and update CREDITS.md. Use before writing any emulation, GPU or game fix, and when asked to bring over an upstream change."
---

# Port an upstream fix

AGENTS.md, "Upstream first": look in Lichii first, then in Eglantine, and port rather than rewrite.

| Upstream | Repository | Branch |
|---|---|---|
| Lichii, ours | https://github.com/zortos293/Lichii | `nx2` |
| Eglantine, Lichii's | https://github.com/Eglantine-Community/Eglantine (older docs say Djihads80/Eglantine) | `nx2` |

This repository's `upstream` branch mirrors Lichii `nx2` and only `upstream-sync.yml` moves it; a
whole-upstream merge goes through that sync pull request (docs/LANES.md). Never commit to `upstream`.

## 1. Find it

- Remotes: `git remote -v`, then add what is missing and fetch:
  `git remote add lichii https://github.com/zortos293/Lichii.git`,
  `git remote add eglantine https://github.com/Eglantine-Community/Eglantine.git`,
  `git fetch lichii`, `git fetch eglantine`.
- Search both, open and closed, by the game (its program id is in AGENTS.md's table) and by symptom:

  ```sh
  gh pr list -R zortos293/Lichii --state all --search "<symptom or game>" --limit 50
  gh issue list -R zortos293/Lichii --state all --search "<symptom or game>" --limit 50
  gh pr list -R Eglantine-Community/Eglantine --state all --search "<symptom or game>" --limit 50
  git log --oneline lichii/nx2 eglantine/nx2 --grep "<keyword>" -i
  git log --oneline -S '<distinctive code>' lichii/nx2 -- src/<area>
  git branch -r --no-merged lichii/nx2 --list 'lichii/*'
  ```

- Read the diff and the discussion (`gh pr diff <n> -R <repo>`, `gh pr view <n> -R <repo> --comments`),
  not only the title: does it address the root cause your log shows, on which GPU and driver, and did
  a later pull request revert or fix it?
- Already here? `git branch -a --contains <sha>`, `git cherry -v origin/main lichii/nx2`, or
  `git log -S '<a line of the fix>' origin/main`.

## 2. Port it

- Branch from `main`: `git switch -c port/lichii-<n>-<topic> origin/main` (or `port/eglantine-...`).
- Merged commits: `git cherry-pick -x <sha>...` keeps the author and records the source; a merge
  commit needs `-m 1`.
- An unmerged pull request, or one that does not apply: `gh pr diff <n> -R <repo> | git apply -3`,
  resolve, and keep the author: `git commit --author "<name> <email>"` (from
  `gh pr view <n> -R <repo> --json author,commits`), or your own authorship with a
  `Co-authored-by: <name> <email>` trailer when you changed the fix.
- The body cites the source, for example `Ported from zortos293/Lichii#<n>`, and says what you changed
  and why. Keep SPDX headers and copyright lines as they are.
- An upstream pull request we carry before upstream merges it gets a line in `CREDITS.md`.
- Port the smallest unit that fixes it: no refactors on the way. Port upstream's follow-up fixes to
  the same code with it.

## 3. Check it and hand it over

- `bash scripts/run-tests.sh` or `powershell -ExecutionPolicy Bypass -File scripts/run-tests.ps1`;
  a port that changes speed is measured like any change (the `perf-change` skill).
- The pull request's "Upstream" section links the upstream pull request or commit and credits the
  author; the subject is `area: change`.
- A fix of ours that is not specific to integrated GPUs or to our switches goes back to
  zortos293/Lichii as well (AGENTS.md, "Send fixes back").
- Ask before pushing, opening the pull request, or commenting upstream.
