# Bringing AI coding agents to Guarana: research, November 2026

What works, as of 2026-11-13, to bring autonomous AI coding agents to an open-source repository and
get their pull requests merged. Free ones come first. The ranking is ours. Every row links its
source, and "third-party" marks a figure that only blogs or aggregators give, not the vendor. What
the repository carries is in [README.md](README.md).

## Ranked

| # | Channel | What it gives Guarana | What the owner must do | Cost |
|---|---|---|---|---|
| 1 | `AGENTS.md` and scoped `agent-ready` issues | Every major agent reads the same rules, and an issue with a checkable acceptance test is what decides whether an agent's pull request lands | Nothing more: both exist. Keep the acceptance tests checkable | Free |
| 2 | A claim protocol (`/claim`, released after 72 h) | Several agents can work without two of them doing the same issue | Merge [agent-claims.yml](../../.github/workflows/agent-claims.yml) | Free (Actions minutes) |
| 3 | GitHub Copilot cloud agent (formerly "coding agent"), and Claude and Codex in Agent HQ | The largest pool of agents: assign an issue and a pull request comes back. Contributors run it on their forks with their own plan | For the main repository: a paid Copilot plan with the agent turned on. Claude and Codex in Agent HQ: Pro+ or Enterprise | Premium requests from the plan of whoever assigns the issue |
| 4 | Gemini CLI GitHub Action (`@gemini-cli`) | The only agent here that a maintainer can call from a comment at no cost, on Google AI Studio's free quota | Add a `GEMINI_API_KEY` secret | Free tier |
| 5 | Claude Code GitHub Action (`@claude`) | Answers, reviews and pushes a branch with a prefilled pull request link; strong on large C++ changes | Install the Claude GitHub App and add `ANTHROPIC_API_KEY` or `CLAUDE_CODE_OAUTH_TOKEN` (`/install-github-app` does both) | API usage, or a Pro or Max plan |
| 6 | OpenAI Codex cloud, `@codex review` | Pull request reviews that follow review rules in `AGENTS.md` | Connect the repository in Codex settings and turn on code review | A ChatGPT plan with cloud features (not Free or Go) |
| 7 | Google Jules | Asynchronous fixes: a `jules` label on an issue starts a task | Install the Jules GitHub app on the repository | Free tier: 15 tasks a day, 3 at once (third-party) |
| 8 | GitHub Agentic Workflows (`gh aw`) | Markdown workflows compiled to Actions: triage, CI doctor, docs upkeep, read-only by default | Install the `gh aw` extension; a Copilot license for the default engine, or another engine's key | Copilot license or engine key |
| 9 | MCP registries (official, Smithery, Glama) | Testers find the debug MCP server from their MCP client | Publish the bundle (steps in README.md) | Free |
| 10 | Cursor cloud agents (formerly "background agents") | `@cursor` in an issue or pull request comment starts an agent | Cursor's GitHub app on the repository and a paid Cursor plan; contributors use their own | Paid plan |
| 11 | OpenHands resolver, mini-swe-agent | Agents that run with any model key, a local model included | Nothing unless the resolver workflow is installed (it needs `LLM_API_KEY`, `LLM_MODEL` and a token) | Own key, or free with a local model |
| 12 | `llms.txt` | Context for agents and web chats that are told to read it | Nothing: [site/llms.txt](../../site/llms.txt) and `llms-full.txt` exist | Free |
| 13 | Cash bounties and leaderboards | Attention, and pull request floods | Nothing now: credit merged work, not attempts | - |

## Findings

1. **`AGENTS.md` is the lever.** It is a Linux Foundation standard now: OpenAI and Anthropic gave it
   to the new Agentic AI Foundation on 2025-12-09, together with MCP and goose
   ([OpenAI](https://openai.com/index/agentic-ai-foundation/), [agents.md](https://agents.md/)).
   Codex code review looks for `AGENTS.md` files and follows the review guidance in the one closest
   to the code ([OpenAI](https://developers.openai.com/codex/integrations/github)). One rulebook
   serves every agent, which is how this repository is already set up.
2. **Copilot cloud agent is the biggest pool, and it needs setup steps from the repository.** You
   assign an issue to Copilot from the Assignees menu, the API or `gh`
   ([GitHub](https://docs.github.com/en/copilot/how-tos/use-copilot-agents/cloud-agent/create-a-pr)).
   Its environment comes from `.github/workflows/copilot-setup-steps.yml`, whose job must be named
   `copilot-setup-steps`. Only `steps`, `permissions`, `runs-on`, `services`, `snapshot` and
   `timeout-minutes` (59 at most) are used, the file must be on the default branch, and the agent's
   firewall does not work on Windows or self-hosted runners
   ([GitHub](https://docs.github.com/en/copilot/how-tos/copilot-on-github/customize-copilot/customize-cloud-agent/customize-the-agent-environment)).
   Agent HQ adds Claude and Codex as assignees, in public preview for Copilot Pro+ and Enterprise,
   at one premium request per session
   ([GitHub](https://github.blog/news-insights/company-news/pick-your-agent-use-claude-and-codex-on-agent-hq/)).
3. **Actions-hosted agents spend the owner's key, so only maintainers may call them.**
   `anthropics/claude-code-action` (v1.0.275, released 2026-11-12) runs only for users with write
   access. Its default sign-in uses the Claude GitHub App through OIDC (`id-token: write`), and it
   pushes a branch with a prefilled pull request link instead of opening the pull request itself
   ([action](https://github.com/anthropics/claude-code-action),
   [security](https://github.com/anthropics/claude-code-action/blob/main/docs/security.md),
   [FAQ](https://github.com/anthropics/claude-code-action/blob/main/docs/faq.md)).
   `google-github-actions/run-gemini-cli` (v0.1.22, 2026-04-24) takes an AI Studio key "with
   generous free-of-charge quotas". Its dispatch example answers only `OWNER`, `MEMBER` and
   `COLLABORATOR` comments ([action](https://github.com/google-github-actions/run-gemini-cli)).
4. **GitHub Agentic Workflows went GA on 2026-10-28.** Workflows are Markdown files that
   `gh aw compile` turns into `.lock.yml` Actions workflows. Agents run read-only in a sandbox
   behind a firewall, and their writes pass through "safe outputs" and a threat-detection job. The
   default Copilot CLI engine needs a Copilot license
   ([changelog](https://github.blog/changelog/2026-10-28-github-agentic-workflows-generally-available),
   [preview](https://github.blog/changelog/2026-02-13-github-agentic-workflows-are-now-in-technical-preview/)).
   It is the natural next step for triage and upkeep, not for code changes.
5. **The MCP registry needs a package, not a script.** The official registry (`mcp-publisher`
   v1.8.9; schema `2025-12-11`) authenticates `io.github.<user>/...` names with GitHub, and lists
   npm, PyPI, NuGet, OCI or MCPB packages. An MCPB bundle must be a GitHub or GitLab release asset
   with "mcp" in its URL and a `fileSha256`
   ([package types](https://github.com/modelcontextprotocol/registry/blob/main/docs/modelcontextprotocol-io/package-types.mdx),
   [requirements](https://github.com/modelcontextprotocol/registry/blob/main/docs/reference/server-json/official-registry-requirements.md)).
   For a stdio script that uses only the standard library, a bundle on our own release needs no new
   account. Smithery also takes stdio servers as MCPB bundles
   ([Smithery](https://smithery.ai/docs/build/publish)). Glama lists GitHub repositories and lets
   the owner claim a listing with a `glama.json` that names the maintainers
   ([Glama](https://glama.ai/blog/2025-07-08-what-is-glamajson)).
6. **Codex cloud and Jules are free only on someone's plan.** `@codex review` in a pull request
   comment, or automatic reviews, once the repository is connected in Codex settings
   ([OpenAI](https://developers.openai.com/codex/cloud/code-review)). Free and Go plans have no
   cloud features ([pricing](https://developers.openai.com/codex/pricing)). Jules starts a task
   when an issue gets the `jules` label and its app can see the repository. The free limits are
   third-party figures ([morphllm](https://www.morphllm.com/comparisons/jules-google-coding-agent),
   [rywalker](https://rywalker.com/research/jules)).
7. **Bring-your-own-model agents.** OpenHands' resolver reacts to a `fix-me` label or an
   `@openhands-agent` comment ([docs](https://docs.all-hands.dev/usage/how-to/github-action); the
   secret names come from older [resolver releases](https://pypi.org/project/openhands-resolver/0.3.1)).
   SWE-agent's own README now recommends mini-swe-agent instead
   ([SWE-agent](https://github.com/SWE-agent/SWE-agent)). Cursor's cloud agents start from an
   `@cursor` comment and read `.cursor/environment.json` ([Cursor](https://cursor.com/docs/cloud-agent)).
8. **`llms.txt` is cheap and rarely read.** About 97% of the files in a 137,000-domain sample got no
   requests in May 2026. Among the named AI tools that did request one, GPTBot and Claude Code led
   ([ppc.land on Ahrefs](https://ppc.land/llms-txt-adoption-rises-8-8x-but-97-of-files-get-zero-ai-requests/),
   third-party). Ours is worth keeping because our prompts tell agents to read it. It is not worth
   more work.
9. **Bounties attract floods.** Write-ups from 2026 describe bounty issues with several competing
   pull requests within hours, AI pull request spam, and maintainers adding trust gates. All of
   them are anecdotal
   ([dev.to](https://dev.to/dyclgtm/i-scanned-1500-github-bounties-with-an-ai-agent-the-public-bounty-market-is-broken-in-2026-33pg),
   [case study](https://clawdbytes.pages.dev/article/2026-05-17-i-tried-to-make-claude-make-me-money-on-open-source-bounties.html)).
   Algora attaches money to issues; we did not verify its command syntax in this round. For
   Guarana: no cash bounties. A claim with a timeout (implemented here) gives the "I am on it"
   signal bounty platforms use. The Hall of fame credits merged work only.

## Not verified in this round

- The Copilot plans that include the cloud agent: the docs pages disagree between "all paid plans"
  and "Pro, Pro+, Business and Enterprise".
- Jules's free-tier limits and model, the OpenHands secret names in the current docs, and Cursor
  pricing.
- Whether Codex in Agent HQ and the Codex cloud review use the same quota.
