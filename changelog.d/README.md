# Changelog fragments

A pull request that changes more than documentation adds one small file here instead of a line in
`CHANGELOG.md`, so two pull requests never edit the same line and merging one does not make the
others conflict ([#118](https://github.com/karimalsalah/guarana/issues/118)).

## The file

`changelog.d/<slug>.<section>.md`

- `<slug>`: a short name for the change, lower-case letters, digits and hyphens: `vulkan-cache-flush`.
- `<section>`: one of `added`, `changed`, `fixed`, `removed`, `deprecated`, `security`. It is the
  `### ` heading the entry goes under in the release notes. The sections `CHANGELOG.md` uses today
  are Added, Changed and Fixed.
- Content: the entry, written as it would read in `CHANGELOG.md`: a line that starts with `- `, and
  lines indented by two spaces that continue it. Links inline. No line may start with `#`. One file
  may hold several entries.

Example, `changelog.d/vulkan-cache-flush.fixed.md`:

```md
- Shader cache: the driver cache is flushed once at exit, not on every pipeline
  ([#106](https://github.com/karimalsalah/guarana/issues/106)).
```

`README.md` and files that start with a dot are ignored. Every other file in this folder must be well
formed, or the check fails.

## The tool

`python -I tools/ci/changelog_fragments.py <command>` (standard library only):

| Command | What it does |
|---|---|
| `collect` | Prints the `## [Unreleased]` section of `CHANGELOG.md` with the fragments folded in, after the lines already in each section. `tools/ci/changelog-section.sh Unreleased` and the nightly release notes use it. Changes nothing. |
| `check [--base REF]` | For a pull request: fails when it changes more than documentation (everything `build-check.yml` skips the builds for, plus `VERSION`, `.github/CODEOWNERS` and `changelog.d/`) and adds neither a fragment nor a line under `## [Unreleased]`. `build-check.yml` runs it with the merge commit's first parent as the base. |
| `release [--into NAME]` | Folds the fragments into `CHANGELOG.md` and deletes them. `tools/ci/bump-version.sh` runs it before it moves `[Unreleased]` under the new version. |

Both forms are accepted until the next release: lines already written under `## [Unreleased]` stay
where they are, and a pull request may still add a line there. Prefer a fragment: it cannot conflict
and cannot land inside a released section.
