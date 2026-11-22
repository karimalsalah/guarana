---
name: release
description: "Cut a Guarana release the docs/LANES.md way: the release/vX.Y branch, the CHANGELOG.md section, a vX.Y.Z tag pushed by a person, release-binaries.yml building and publishing with SHA256SUMS.txt, then download every asset and verify its SHA-256. Use when asked to prepare, tag, publish, rebuild or verify a release or release candidate."
---

# Cut a release

`docs/LANES.md` ("Releases", "Hotfixes") is the process; this is its checklist. Versions are
`vX.Y.Z`, pre-releases `vX.Y.Z-rcN`. Ask before every step that publishes or is hard to undo: pushing
a branch or a tag, opening or merging a pull request, running a workflow, editing a release.

## 1. Preflight

- `git fetch origin --tags` and a clean `git status`.
- `main` is green: `gh run list --workflow build-check.yml --branch main --limit 3`.
- What goes in: `git log --oneline <previous tag>..origin/main`. Every user-visible change has a fragment
  in `changelog.d/` or a line under `## [Unreleased]` in `CHANGELOG.md`; its header has the format rules (links inline, `###`
  groups, no other `## ` line inside a version).

## 2. Branch and notes

- A new X.Y: `git switch -c release/vX.Y origin/main`. A patch: `git switch release/vX.Y`, which
  takes only fixes, cherry-picked from `main` (`git cherry-pick -x`) or merged from `hotfix/*`.
- On the release branch, run `python3 -I tools/ci/changelog_fragments.py release` (folds the `changelog.d/`
  fragments into `[Unreleased]` and deletes them), then move the `[Unreleased]` entries into
  `## [X.Y.Z] - YYYY-MM-DD`. A
  pre-release uses a `## [X.Y.Z-rcN]` section if there is one, else `## [X.Y.Z]`.
- `bash tools/ci/changelog-section.sh X.Y.Z` must print the notes: an empty section fails the
  release workflow in its first minute, before anything is built.
- Commit (`docs: release notes for vX.Y.Z`) and land it on `release/vX.Y` through a pull request.

## 3. Tag

```sh
git tag -a vX.Y.Z -m "Guarana vX.Y.Z" <commit on release/vX.Y>
git push origin vX.Y.Z
```

The push must come from a person: a tag pushed with a workflow's token starts no workflow. Push it
only after the owner says so, or hand them the two commands.

## 4. Watch the workflow

- `gh run list --workflow release-binaries.yml --limit 3`, then `gh run watch <id>`; on a failure
  `gh run view <id> --log-failed`.
- A notes failure: fix the section; move the tag (`git tag -f -a vX.Y.Z`, `git push -f origin vX.Y.Z`)
  only if nobody can have downloaded it, else publish the next patch.
- A runner failure or a packaging fix: rebuild the tag with `gh workflow run release-binaries.yml
  --ref vX.Y.Z` (ask first).
- `gh release view vX.Y.Z` must list `guarana-X.Y.Z-<platform>.<ext>` for every platform and
  `SHA256SUMS.txt`; a `-rcN` tag is a pre-release.

## 5. Verify the checksums

```sh
mkdir -p build/release-check/vX.Y.Z && cd build/release-check/vX.Y.Z
gh release download vX.Y.Z --clobber               # every asset: several hundred MB
sha256sum -c SHA256SUMS.txt                        # every line OK
ls | grep -vxF -e SHA256SUMS.txt -e github.sha256 -e files.txt | sort > files.txt
awk '{print $2}' SHA256SUMS.txt | sort | diff - files.txt   # no asset missing from the list
gh api "repos/{owner}/{repo}/releases/tags/vX.Y.Z" \
  --jq '.assets[] | select(.digest != null) | "\(.digest | ltrimstr("sha256:"))  \(.name)"' > github.sha256
sha256sum -c github.sha256                         # GitHub's digest of each upload
```

- Every asset passes both checks and appears in `SHA256SUMS.txt`. In PowerShell,
  `Get-FileHash -Algorithm SHA256 <file>` gives the same hash.
- A mismatch or a missing file is a failed release: report it; do not delete or replace assets.
- Spot-check the contents: `unzip -l guarana-X.Y.Z-windows-x86_64.zip` shows `guarana.exe`,
  `guarana-cli.exe`, `qt.conf` and `plugins/`; `tar -tzf guarana-X.Y.Z-linux-x86_64.tar.gz | head`.
- A local zip from `scripts/package-windows.ps1` has its own `<zip>.sha256`:
  `sha256sum -c <zip>.sha256`.

## 6. After

- Merge the release branch's `CHANGELOG.md` change back into `main` through a pull request.
- Report the tag, the release and run URLs, every asset with its size and hash, and what was checked.
