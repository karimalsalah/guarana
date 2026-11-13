# Lanes

The long-lived branches of karimalsalah/guarana, what may land on each, and what CI does with them. Contributors work on topic branches in their forks and open pull requests; maintainers merge them and push tags.

| Lane | What it is | What lands there | CI |
|---|---|---|---|
| `main` | Integration branch, the default branch | Reviewed pull requests. Experiments only behind default-off switches | `build-check` on every push and pull request; `nightly` when it moved |
| `release/vX.Y` | Stabilization of version X.Y | Fixes only: no features, no new switches | `build-check`; a `vX.Y.Z` tag on it builds and publishes the release |
| `hotfix/*` | An urgent fix for a published version | One fix, branched from the release tag | `build-check` |
| `exp/*` | Experiments | Anything, behind default-off `EDEN_NX2_*` switches | `build-check`, with the Windows test build |
| `upstream` | Mirror of [zortos293/Lichii](https://github.com/zortos293/Lichii) `nx2` | Nothing by hand: only `upstream-sync.yml` moves it | The sync opens a merge pull request into `main` |

## main

- Always green: `build-check` (Linux GCC and Windows clang-cl, `-Werror`, unit and Python tests) passes on every commit. Protect it: require the two build jobs and a review, and allow merges only through pull requests.
- Experiments may land here, off by default, each with a row in [switches.md](switches.md). A switch that shows a measured gain on a verified scene can become the default behaviour in a later pull request, with the measurement. A disproven one is removed, or stays off with the result written in its row.
- `nightly.yml` builds `main` every day at 03:23 UTC if it moved since the last nightly and replaces the rolling [`nightly` pre-release](https://github.com/karimalsalah/guarana/releases/tag/nightly), files and tag. Run it by hand (Actions > nightly > Run workflow, with "force") to rebuild without a new commit. GitHub turns off schedules in a repository without activity for 60 days; the Actions page then has a button to turn them back on.

## Releases

Versions are `vX.Y.Z`; release candidates and other pre-releases are `vX.Y.Z-rcN`.

1. Cut the branch from `main`: `git switch -c release/vX.Y main` and push it. From then on only fixes go in: cherry-picked from `main` (`git cherry-pick -x`), or merged from `hotfix/*`.
2. Write the notes: on the release branch, move the entries under `## [Unreleased]` in `CHANGELOG.md` to a new `## [X.Y.Z] - YYYY-MM-DD` section. `bash tools/ci/changelog-section.sh X.Y.Z` prints what the release will use. A `vX.Y.Z-rcN` tag uses a `## [X.Y.Z-rcN]` section if there is one, else `## [X.Y.Z]`.
3. A maintainer tags the commit and pushes the tag:

   ```sh
   git tag -a vX.Y.Z -m "Guarana vX.Y.Z"
   git push origin vX.Y.Z
   ```

   Tags pushed by a workflow with the repository's token would not start the release workflow, so tags are pushed by people.
4. `release-binaries.yml` checks the notes first: a tag without a non-empty section fails within a minute, before anything is built. Then it builds every platform (Windows clang-cl ThinLTO, with and without PGO; Linux x86_64 and aarch64 AppImage and tar.gz, with and without PGO; macOS arm64) and publishes the GitHub release with all the files, `SHA256SUMS.txt` and a build provenance attestation for every file; with the signing secrets set, the Windows programs and the installer are Authenticode-signed first ([SIGNING.md](SIGNING.md)). Files are named `guarana-<version>-<platform>.<ext>`, for example `guarana-0.2.0-windows-x86_64-pgo.zip`. A tag with a pre-release suffix is published as a pre-release.
5. Merge the release branch's `CHANGELOG.md` change back into `main`.

To rebuild a published tag (a runner failure, a packaging fix that is not in the tag), run Actions > release-binaries > Run workflow with the tag: its release gets the new files and notes. To fix the notes, change `CHANGELOG.md`, move the tag (`git tag -f -a vX.Y.Z`, `git push -f origin vX.Y.Z`) before anyone downloads it, or publish the next patch version. Run from a branch without a tag, the workflow builds artifacts only and publishes nothing.

The release builds run with the update checker off: it asks zortos293/Lichii for releases and only understands upstream's `build-N` tags, so it could offer upstream's builds to this fork's users. `LICHII_BUILD_TAG` is the tag, and the build reports the tag as its version.

## Hotfixes

1. Branch from the release tag: `git switch -c hotfix/<topic> vX.Y.Z`.
2. Open a pull request into `release/vX.Y`. Once it is merged, tag `vX.Y.(Z+1)` on the release branch as above.
3. Forward-port: open a pull request with the same fix into `main` (`git cherry-pick -x`), or explain in the hotfix pull request why `main` does not need it.

## Experiments

- Push `exp/<topic>` to this repository (maintainers), or to your fork with Actions turned on: `build-check` runs on `exp/**`, and its run page has the Windows test build to download.
- Everything an experiment changes is behind a default-off `EDEN_NX2_*` switch with a row in [switches.md](switches.md), so it can merge into `main` and be A/B tested in one build.
- A switch earns a place as default behaviour only with a measured gain on a verified scene. Write the result in its row either way.

## Upstream

`upstream-sync.yml` runs every 6 hours at minute 41 (and by hand):

1. It fetches zortos293/Lichii `nx2` and fast-forwards this repository's `upstream` branch to it. It never force-pushes.
2. If `main` lacks commits of `upstream`, it opens the pull request `upstream` into `main`, or updates its title, body and commit list. Upstream's `#123` references are written as `zortos293/Lichii#123` so they do not point at this repository's numbers.

To merge it:

- **CI**: a pull request opened by a workflow with the repository's token starts no workflows. Close and reopen it to run `build-check` on the merge, unless the `UPSTREAM_SYNC_TOKEN` secret is set (below).
- **No conflicts**: review, wait for CI, then merge with a merge commit. Never squash or rebase it: later syncs need the shared history.
- **Conflicts**: do not resolve them in the GitHub editor, which would commit into `upstream`. Resolve them on a branch from `main` and open that pull request instead; the sync pull request is marked merged once `main` contains its commits.

  ```sh
  git switch -c merge/upstream-YYYYMMDD origin/main
  git merge origin/upstream    # keep both sides; list each resolution in the pull request
  git push origin merge/upstream-YYYYMMDD
  ```

- After either: run the verified scenes if upstream touched frame pacing, the shader recompiler or the Vulkan backend, check that the rows in [switches.md](switches.md) still hold, and add the upstream range to `CHANGELOG.md`.

### When the sync stops

- **"GitHub refused the push because upstream changed .github/workflows"**: the repository's token may not push commits that change workflow files. Push the mirror yourself:

  ```sh
  git fetch https://github.com/zortos293/Lichii.git nx2
  git push origin FETCH_HEAD:refs/heads/upstream
  ```

  or add the optional `UPSTREAM_SYNC_TOKEN` secret: a fine-grained token, or a GitHub App's, with Contents, Pull requests and Workflows write on this repository. With it the sync also pushes workflow changes, and its pull requests start CI on their own. Nothing else needs a secret but the optional release signing ([SIGNING.md](SIGNING.md)).
- **"The upstream branch did not fast-forward"**: upstream rewrote `nx2`, or someone committed to `upstream`. Look at both before you overwrite anything; then reset the mirror with `git push --force origin FETCH_HEAD:refs/heads/upstream` after the fetch above.
- **"Could not open the pull request"**: allow GitHub Actions to create pull requests (Settings > Actions > General > Workflow permissions), or set `UPSTREAM_SYNC_TOKEN`.

## Pull request test builds

`build-check` uploads the Windows build of every pull request that changes code as `guarana-pr<N>-<commit>-windows-x86_64`, kept for 14 days, and links it in the run summary. `pr-build-comment.yml` then links it in a comment on the pull request, forks included, and updates that comment on each new run. It is a Release build of the pull request merged into its base, without the release builds' ThinLTO and PGO. Downloading needs a GitHub account. Pull requests that change only documentation, the website, issue forms, the label list or benchmark rows skip the builds.

## Other workflows

- `labels.yml` creates and updates the labels in `.github/labels.yml` when that file changes on `main`. It never deletes a label.
- `pages.yml` publishes `site/` to GitHub Pages when it changes on `main`.
- `label-issues.yml` labels new issues from their form answers (game, GPU vendor, integrated GPU family, handheld, operating system, regression).
- `ingest-reports.yml` turns `device-report` issues into `site/data/community.json` (the community leaderboard on the benchmarks page) through one bot pull request on `bot/community-reports`, and comments on each report with its parsed row. See [FEEDBACK.md](FEEDBACK.md).
- `triage.yml` labels new issues by game, GPU and symptom, asks once for a log when a crash or bug report has none, and adds `agent-ready` to agent tasks whose acceptance test is checkable. It never closes, locks or removes anything.
- `promo-render.yml` renders the 20-second promo in `site/promo/` to MP4 and GIF artifacts.
- No workflow closes or marks issues as stale.

## Repository settings these need

- Settings > Actions > General > Workflow permissions: "Allow GitHub Actions to create and approve pull requests", for the sync pull request.
- Settings > Pages > Build and deployment > Source: "GitHub Actions".
- Settings > General > Features: Discussions on (the issue forms link to them).
- Settings > Security: "Private vulnerability reporting" on (SECURITY.md and CODE_OF_CONDUCT.md rely on it).
- Branch protection or rulesets: `main` and `release/*` require the `build-check` jobs and a review. Tags `v*` may be created only by maintainers. Nobody but the sync workflow pushes `upstream`.
- Settings > Secrets and variables > Actions: the six signing secrets, once releases should be Authenticode-signed ([SIGNING.md](SIGNING.md), "Turn signing on"). Builds and provenance need no secret.
