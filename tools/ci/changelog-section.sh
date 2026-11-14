#!/usr/bin/env bash
# usage: changelog-section.sh <name>
# Prints the body of the exact "## [<name>]" or "## [<name>] - <date>" section. For Unreleased, the
# changelog.d fragments are folded in (tools/ci/changelog_fragments.py collect).
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."
name="${1:?usage: changelog-section.sh <name>}"

if [[ "$name" == Unreleased ]]; then
  exec python3 -I tools/ci/changelog_fragments.py collect
fi

awk -v name="$name" '
  !found && ($0 == "## [" name "]" || index($0, "## [" name "] - ") == 1) {
    found = 1
    next
  }
  found && /^## / { exit }
  found { print }
' CHANGELOG.md
