# Changelog

All notable changes to Eglantine are documented here. Each release's notes are taken from its
`## [X.Y.Z]` section (release candidates use the section of the version they are a candidate for).
New entries go under `## [Unreleased]`; `tools/ci/bump-version.sh` turns them into a version section.

## [Unreleased]

- Frame-time hitch experiments, all off by default: `EDEN_NX2_HITCH_FINISH_TRACE` (callers of `Scheduler::Finish`), `EDEN_NX2_HITCH_GC_BATCH` and `EDEN_NX2_HITCH_GC_CLEAN_FIRST` (texture collector downloads), `EDEN_NX2_HITCH_UMA_BUDGET_MIB` (integrated device budget cap). Analysis in `docs/hitches.md`.

## [0.1.0]

- Initial release candidate
