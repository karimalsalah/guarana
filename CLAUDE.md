@AGENTS.md

## Claude Code

- While the owner runs a build with `EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545`, use the `emulator-debug` MCP server that `.mcp.json` registers (`python -X utf8 -I tools/nx2/debug_mcp.py --server 127.0.0.1:6545`) and its tools instead of `--call` loops. One request at a time while the owner plays.
- Hand read-only surveys (upstream pull requests and issues, log reading, finding a switch's readers) to subagents; keep edits and reviews in the main conversation.
- Do not start builds, benchmarks or the emulator, in the foreground or the background, unless asked: a compile on the same machine spoils a `tools/bench` run.
- Before you finish, run the unit and Python tests that cover the change, and check `git status` for anything under "Never commit" in AGENTS.md.
