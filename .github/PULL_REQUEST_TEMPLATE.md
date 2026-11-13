## What and why

<!-- What changes, and the problem it solves. Link the issue: "Closes #123". -->

## Measured result

<!-- Required for performance, pacing, memory and power changes; otherwise write "Not a performance change".
     Paired runs in one session, baseline next to the change, on a verified scene (tools/bench rows with
     "scene_ok": true). Paste or link the rows, or the output of `python -I tools/bench/bench.py compare`. -->

| Device (CPU / GPU / driver / power) | Scene | Metric | Before | After | Runs |
| --- | --- | --- | --- | --- | --- |
| | | | | | |

## Switches added or changed

<!-- Each EDEN_NX2_* switch with its values and default (off). Write "None" if there are none.
     Every one needs a row in docs/switches.md in this pull request. -->

## Risk

<!-- What could render or behave differently, on which GPUs or drivers, and how to turn it off. -->

## Upstream

<!-- What you found in zortos293/Lichii (nx2, open and closed pull requests, issues) and in Eglantine,
     with links. If this ports someone's work: the original pull request or commit, and the author
     credited in the commit (Co-authored-by) and in CREDITS.md. If this fix belongs upstream too,
     link the upstream pull request or say you will open one. -->

## Checklist

- [ ] No keys, firmware, game files or assets, dumps, shader caches, saves, or logs with personal paths
- [ ] New behaviour that has no verified gain yet is behind an `EDEN_NX2_*` switch that is off by default
- [ ] `docs/switches.md` is updated for every switch added, changed or removed
- [ ] Tests added or updated, and they pass (`-DLICHII_TESTS=ON`, `ctest`; the Python tests for tools)
- [ ] Commit subjects are `area: change` in the imperative; measured numbers are in the commit body
- [ ] `CHANGELOG.md` has an entry under `[Unreleased]`
- [ ] Upstream checked (see above)
