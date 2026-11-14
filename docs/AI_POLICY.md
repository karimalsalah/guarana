# AI policy

Guarana is meant to be worked on by people and AI agents together: [AGENTS.md](../AGENTS.md) gives
agents the rules of the tree, `agent-ready` issues give them scoped work, and the debug MCP server
lets them see a running game. AI-assisted contributions are welcome. This policy keeps them
accountable.

**In short:**

1. **Disclose** AI help with an `Assisted-by: <tool>` trailer.
2. **A human owns every pull request**, and can explain every line of it.
3. **Performance claims need measured evidence**: bench rows, not reasoning.
4. **Spam pull requests are closed** without review.
5. **No AI-generated game assets.**

## 1. Disclose

Add one trailer per tool to every commit that an AI tool helped write:

```text
renderer_vulkan: hold the last predicted frame for zero spacing

<what changed and why>

Assisted-by: Claude Code
Assisted-by: GitHub Copilot
```

- Name the tool, for example `Claude Code`, `Codex`, `Cursor` or `GitHub Copilot`. Adding the model
  is welcome: `Assisted-by: Cursor (<model name>)`.
- Say in the pull request description what the tool did: wrote a first draft, wrote the tests, found
  the bug, reviewed the change.
- Keep any trailer your tool adds by itself, such as `Co-authored-by:`, and add `Assisted-by:` as
  well.
- Single-line editor completions need no trailer. Anything larger does.

Disclosure is not a mark against a contribution. Hiding it is.

## 2. A human owns every pull request

- A person opens the pull request from their own account and is its owner.
- The owner has read the whole diff, can explain why it is correct, and answers review questions in
  their own words. "The AI wrote it" is not an answer.
- Agents may push to a draft pull request. Marking it ready for review means the owner has checked
  it and stands behind it.
- Accounts that open pull requests with no human owner behind them are not accepted.

## 3. Evidence

- **Performance:** a paired A/B from one session, baseline and change back to back on the same
  machine and scene, made with `tools/bench`, both rows valid with `"scene_ok": true`. See
  [BENCHMARKING.md](BENCHMARKING.md#evidence-for-a-pull-request).
- **Correctness:** tests under `src/tests` or next to the tool you changed, and for rendering
  changes, screenshots or frame captures of the affected passes. Say what you checked in a running
  game, and on which device, separately from what the automated checks covered.
- **Facts:** link the source for every claim about hardware, drivers or upstream. A number a tool
  produced without a measurement behind it does not go into code, docs or pull requests.
- **Upstream first:** check whether Lichii already has the fix before writing one
  ([AGENTS.md](../AGENTS.md)).

## 4. Spam

Closed without review:

- pull requests whose owner has not read them or cannot explain them;
- mass or cosmetic changes nobody asked for, such as renames, reformatting or comment rewrites across
  many files;
- "optimizations" without bench rows;
- generated issues or bug reports without a real reproduction on a real device.

Repeated spam gets the account blocked from the repository.

## 5. Assets and content

- **No AI-generated game assets:** no generated textures, models, audio, fonts or art that imitate,
  replace or are derived from content of a game.
- No Nintendo characters, logos or artwork in project material, generated or not.
- Project art and website assets made with AI tools are disclosed like code.

## Data your tools see

- Never give keys, firmware, game files, or anything extracted from them to an AI tool or any other
  outside service.
- Logs contain paths from your machine, and frame captures contain game data. Read logs before
  sharing them, and keep captures on your machine.
- The debug server listens on loopback only. Do not expose it to a network or tunnel it.

## Licensing

You must have the right to submit your contribution under GPL-3.0-or-later. Do not submit output
that reproduces code under an incompatible license, or leaked or proprietary material of any kind.

## Questions

Ask in [Discussions](https://github.com/karimalsalah/guarana/discussions). How to contribute in
general: [CONTRIBUTING.md](../CONTRIBUTING.md).
