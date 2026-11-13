# Security policy

## Reporting a vulnerability

Report it privately, never in a public issue, pull request or discussion: use GitHub's private vulnerability reporting at <https://github.com/karimalsalah/guarana/security/advisories/new> (the repository's Security tab, "Report a vulnerability"). Only the maintainers can read the report.

Include:

- the version (`vX.Y.Z`, a nightly's date and commit, or a commit hash) and the platform;
- what an attacker can do, and what they need first (a file the user opens, a mod, a network position, local access);
- steps to reproduce, and a proof of concept if you have one.

Do not attach keys, firmware, game files or dumps of them, and do not send a copyrighted game file as a test case. Describe it instead, or build a minimal file of your own.

The maintainers are volunteers. We aim to acknowledge a report within 7 days, agree on a fix and a disclosure date with you, publish a fix in a hotfix release (docs/LANES.md), and credit you in the advisory unless you prefer otherwise.

## Supported versions

| Version | Fixed |
|---|---|
| The latest `vX.Y.Z` release | Yes, in a hotfix release |
| `main` and the `nightly` pre-release | Yes |
| Older releases | No: update to the latest |

## In scope

- Memory corruption or code execution through anything the emulator loads: game containers, updates and DLC, mods and cheats, shader and pipeline caches, saves, `nx2_switches.txt`, configuration files.
- The debug server (`EDEN_NX2_DEBUG_SERVER`). It is off unless set, listens on loopback only, and writes only under its dump directory ([docs/nx2/debug-mcp.md](docs/nx2/debug-mcp.md)): a way to reach it from another machine, to read or write files outside that directory, or to read host memory through it is a vulnerability.
- The build and release pipeline: the workflows in `.github/workflows`, the published release files, their `SHA256SUMS.txt`, build provenance attestations and Authenticode signatures ([docs/SIGNING.md](docs/SIGNING.md) says how to check them), and anything that lets a pull request from a fork run code with this repository's write permissions.
- Network features, where a build enables them.

## Not vulnerabilities

- A game that crashes the emulator without memory corruption: open a crash issue.
- Anything that needs the attacker to already control your user account or the emulator's files.
- Questions about keys, firmware, encryption or copy protection of games. They are out of scope, and see [LEGAL.md](LEGAL.md).
- Bugs in upstream projects (Lichii, Eglantine, Eden) or in third-party libraries that do not affect Guarana: report them to that project. When they affect us too, report here as well and we will coordinate.
