#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The legacy wall: CONTRIBUTORS.md, site/data/legacy.json and the README's Hall of fame avatars.

  python -I tools/ci/legacy.py build [--repo OWNER/NAME] [--since DATE] [--root DIR]
  python -I tools/ci/legacy.py publish [--dry-run]

Everyone who is credited for good: the authors of Guarana's commits (as the GitHub API names them)
and the humans named in `Co-authored-by:` trailers. Bots and AI tools are listed apart, as tools,
not as people: the author's login or a trailer that looks like a bot or an AI tool
("capy-ai[bot]", "Claude Opus 5.5 <noreply@anthropic.com>", "Copilot", an `Assisted-by:` line).
Only commits since SINCE count. Guarana is a fork, so the history before it belongs to the
upstream projects and is credited in CREDITS.md and git itself; counting it here would put
thousands of upstream authors on Guarana's wall. Merge commits are skipped. Trailer e-mail
addresses are read to find a login (GitHub's noreply form, or the address of a commit author) and
are never written anywhere.

build writes the three outputs in the working tree. publish does the same against the default
branch on a branch of its own (bot/legacy), as one commit and one pull request, like
tools/ingest/ingest.py publish does for the leaderboard; it changes nothing when the output is
already on the default branch. Standard library only.
"""

import argparse
import datetime
import json
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
INGEST = ROOT / "tools" / "ingest"
if str(INGEST) not in sys.path:
    sys.path.insert(0, str(INGEST))  # -I leaves the script's directory off sys.path.

import github_api  # noqa: E402

SINCE = "2026-11-13T00:00:00Z"  # Guarana's first commit; see the module docstring
BRANCH = "bot/legacy"
DATA_PATH = "site/data/legacy.json"
CONTRIBUTORS_PATH = "CONTRIBUTORS.md"
README_PATH = "README.md"
START = "<!-- legacy:start -->"
END = "<!-- legacy:end -->"
SCHEMA_VERSION = 1
README_AVATARS = 60
PR_MARKER = "<!-- guarana-legacy-pr -->"
PULL_TITLE = "docs: Hall of fame from the git history"
BOT_IDENTITY = ("github-actions[bot]", "41898282+github-actions[bot]@users.noreply.github.com")

LOGIN = re.compile(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,38})")
NOREPLY = re.compile(r"(?i)^(?:\d+\+)?([A-Za-z0-9][A-Za-z0-9-]*(?:\[bot\])?)@users\.noreply\.github\.com$")
CO_AUTHOR = re.compile(r"(?im)^[ \t]*co-authored-by:[ \t]*(.*?)[ \t]*<([^>\r\n]*)>[ \t]*$")
ASSISTED_BY = re.compile(r"(?im)^[ \t]*assisted-by:[ \t]*([^\r\n]+?)[ \t]*$")

# (pattern over the name and the address, the name the tool is listed under). First match wins.
TOOLS = (
    (re.compile(r"(?i)claude|anthropic"), "Claude"),
    (re.compile(r"(?i)copilot"), "GitHub Copilot"),
    (re.compile(r"(?i)capy"), "Capy"),
    (re.compile(r"(?i)codex|chatgpt|openai|\bgpt[- ]?\d"), "OpenAI Codex / ChatGPT"),
    (re.compile(r"(?i)gemini|google[- ]?ai|jules"), "Gemini"),
    (re.compile(r"(?i)cursor[- ]?(?:agent|ai|bot)|\bcursoragent\b"), "Cursor"),
    (re.compile(r"(?i)\baider\b"), "Aider"),
    (re.compile(r"(?i)\bcline\b"), "Cline"),
    (re.compile(r"(?i)windsurf|cascade"), "Windsurf"),
    (re.compile(r"(?i)\bdevin\b"), "Devin"),
    (re.compile(r"(?i)deepseek"), "DeepSeek"),
    (re.compile(r"(?i)github-actions"), "GitHub Actions"),
    (re.compile(r"(?i)dependabot"), "Dependabot"),
    (re.compile(r"(?i)renovate"), "Renovate"),
)


def tool_name(*texts):
    """The tool a name or an address belongs to, or None. A name with [bot] is a tool as well."""
    joined = " ".join(text for text in texts if text)
    for pattern, name in TOOLS:
        if pattern.search(joined):
            return name
    bot = re.search(r"([A-Za-z0-9][A-Za-z0-9._-]*)\[bot\]", joined)
    return bot.group(1) if bot else None


def clean_name(name):
    return re.sub(r"\s+", " ", (name or "").replace("<", "").replace(">", "")).strip()[:80]


def day(stamp):
    return (stamp or "")[:10]


class Ledger:
    """Collects people and tools from commits."""

    def __init__(self):
        self.people = {}  # key -> record
        self.tools = {}   # name -> commits it helped with
        self.emails = {}  # lower-cased address of a commit author -> login
        self.commits = 0
        self.as_of = ""

    def person(self, login, name, date):
        key = login.casefold() if login else "name:" + clean_name(name).casefold()
        record = self.people.setdefault(key, {"login": login or None, "name": None if login else clean_name(name),
                                              "commits": 0, "coauthored": 0, "first": date})
        if date and (not record["first"] or date < record["first"]):
            record["first"] = date
        return record

    def tool(self, name):
        self.tools[name] = self.tools.get(name, 0) + 1


def learn_addresses(commits, ledger):
    """The e-mail address of each commit author the API could name, to find co-authors by address."""
    for commit in commits:
        author = commit.get("author") or {}
        email = ((commit.get("commit") or {}).get("author") or {}).get("email") or ""
        if author.get("login") and email and not NOREPLY.match(email):
            ledger.emails[email.casefold()] = author["login"]


def resolve_login(name, email, ledger):
    match = NOREPLY.match(email or "")
    if match:
        return match.group(1)
    return ledger.emails.get((email or "").casefold())


def add_commit(commit, ledger):
    if len(commit.get("parents") or []) > 1:
        return
    info = commit.get("commit") or {}
    message = info.get("message") or ""
    date = day((info.get("author") or {}).get("date") or (info.get("committer") or {}).get("date"))
    ledger.commits += 1
    ledger.as_of = max(ledger.as_of, date)
    author = commit.get("author") or {}
    login = author.get("login")
    author_name = (info.get("author") or {}).get("name") or ""
    is_bot = author.get("type") == "Bot" or bool(login and "[bot]" in login)
    tool = None
    if is_bot or not login:
        tool = tool_name(login, author_name) or (clean_name(login or author_name).replace("[bot]", "") if is_bot else None)
    if tool:
        ledger.tool(tool)
    elif login and LOGIN.fullmatch(login):
        ledger.person(login, author_name, date)["commits"] += 1
    elif author_name:
        ledger.person(None, author_name, date)["commits"] += 1
    seen = {tool} if tool else set()
    for name, email in CO_AUTHOR.findall(message):
        name = clean_name(name)
        if not name and not email:
            continue
        kind = tool_name(name, email)
        if kind:
            if kind not in seen:
                ledger.tool(kind)
                seen.add(kind)
            continue
        co_login = resolve_login(name, email, ledger)
        if co_login and not LOGIN.fullmatch(co_login):
            continue
        if co_login and co_login.casefold() == (login or "").casefold():
            continue  # the author named themself
        if not co_login and (not name or name.casefold() == author_name.casefold()):
            continue  # nameless, or the author again
        ledger.person(co_login, name, date)["coauthored"] += 1
    for raw in ASSISTED_BY.findall(message):
        kind = tool_name(raw) or clean_name(re.sub(r"[<(].*", "", raw))
        if kind and kind not in seen:
            ledger.tool(kind)
            seen.add(kind)


def build_ledger(commits):
    ledger = Ledger()
    commits = list(commits)
    learn_addresses(commits, ledger)
    for commit in commits:
        add_commit(commit, ledger)
    return ledger


def document(ledger, repo, since):
    people = sorted(ledger.people.values(), key=lambda p: (p["first"] or "9999", (p["login"] or p["name"] or "").casefold()))
    entries = []
    for person in people:
        entry = {"login": person["login"], "name": person["name"], "commits": person["commits"],
                 "coauthored": person["coauthored"], "first": person["first"]}
        entries.append(entry)
    tools = [{"name": name, "commits": count} for name, count in
             sorted(ledger.tools.items(), key=lambda item: (-item[1], item[0].casefold()))]
    return {"schema_version": SCHEMA_VERSION, "repo": repo, "since": day(since), "as_of": ledger.as_of,
            "commits": ledger.commits, "people": entries, "tools": tools}


def dump(doc):
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def person_label(entry):
    return entry["login"] or entry["name"]


def avatar_html(entry, size):
    login = entry["login"]
    return (f'<a href="https://github.com/{login}"><img src="https://github.com/{login}.png?size={size * 2}" '
            f'width="{size}" height="{size}" alt="{login}" title="{login}"></a>')


def md_cell(text):
    return re.sub(r"[|\r\n]", " ", str(text)).replace("<", "&lt;").replace(">", "&gt;")


def contributors_md(doc):
    site = f"https://{doc['repo'].split('/')[0].lower()}.github.io/{doc['repo'].split('/')[1]}"
    people, tools = doc["people"], doc["tools"]
    lines = [
        "# Contributors",
        "",
        "<!-- Generated by tools/ci/legacy.py (.github/workflows/legacy.yml). Do not edit by hand. -->",
        "",
        f"Everyone who is credited for good in Guarana: {len(people)} "
        f"{'person' if len(people) == 1 else 'people'}, from {doc['commits']} commits since {doc['since']}. "
        f"The same list, with faces, is the [Hall of fame]({site}/legacy.html) on the website.",
        "",
        "Your name is added here when a pull request of yours is merged, or when a merged commit names you in a "
        "`Co-authored-by:` trailer. It is rebuilt daily from the git history, and it stays.",
        "",
        "## People",
        "",
        "| Contributor | Commits | Co-authored | First on |",
        "|---|---:|---:|---|",
    ]
    for entry in people:
        who = f"[{entry['login']}](https://github.com/{entry['login']})" if entry["login"] else md_cell(entry["name"])
        lines.append(f"| {who} | {entry['commits']} | {entry['coauthored']} | {entry['first'] or ''} |")
    if not people:
        lines.append("| Nobody yet: yours can be the first | | | |")
    lines += [
        "",
        "## Tools",
        "",
        "AI tools and bots that helped are listed here, apart from the people: a person owns every pull request, "
        "and the tool is named by its `Co-authored-by:` or `Assisted-by:` line.",
        "",
    ]
    if tools:
        lines += ["| Tool | Commits it helped with |", "|---|---:|"]
        lines += [f"| {md_cell(tool['name'])} | {tool['commits']} |" for tool in tools]
    else:
        lines.append("No tool has been named yet.")
    lines += [
        "",
        "## What counts",
        "",
        f"Commits since {doc['since']}, the first day of Guarana. Before that the history is the upstream "
        "projects' (Lichii, Eglantine, Eden, yuzu), credited in [CREDITS.md](CREDITS.md) and in git itself. "
        "Merge commits are not counted. Trailer e-mail addresses are only used to find a GitHub login and are not "
        "published.",
        "",
    ]
    return "\n".join(lines)


def readme_section(doc):
    people = [entry for entry in doc["people"] if entry["login"]]
    site = f"https://{doc['repo'].split('/')[0].lower()}.github.io/{doc['repo'].split('/')[1]}"
    lines = [START, "<!-- Generated by tools/ci/legacy.py. Do not edit between the markers. -->", ""]
    if people:
        lines.append('<p align="center">')
        lines += ["  " + avatar_html(entry, 48) for entry in people[:README_AVATARS]]
        lines += ["</p>", ""]
        more = len(doc["people"]) - min(len(people), README_AVATARS)
        tail = f", {more} of them not shown here" if more > 0 else ""
        lines.append(f"**{len(doc['people'])} {'person' if len(doc['people']) == 1 else 'people'} credited for good**{tail}. "
                     f"Everyone: [CONTRIBUTORS.md](CONTRIBUTORS.md) and the [Hall of fame]({site}/legacy.html).")
    else:
        lines.append(f"The wall is empty, so the first name on it can be yours: [Hall of fame]({site}/legacy.html).")
    lines += ["", END]
    return "\n".join(lines)


def apply_readme(text, section):
    """text with the part between the markers replaced; None when it has no markers."""
    nl = "\r\n" if "\r\n" in text else "\n"
    plain = text.replace("\r\n", "\n")
    start, end = plain.find(START), plain.find(END)
    if start < 0 or end < start:
        return None
    return (plain[:start] + section + plain[end + len(END):]).replace("\n", nl)


def outputs(doc, readme_text):
    """{path: content} of the generated files; README is left out when it has no markers."""
    files = {DATA_PATH: dump(doc), CONTRIBUTORS_PATH: contributors_md(doc)}
    readme = apply_readme(readme_text or "", readme_section(doc))
    if readme is not None:
        files[README_PATH] = readme
    return files


# ---------------------------------------------------------------------------------------------
# GitHub and git

def fetch_commits(github, repo, since):
    return github.paginate(f"/repos/{repo}/commits", since=since)


def build_document(github, repo, since):
    return document(build_ledger(fetch_commits(github, repo, since)), repo, since)


def write_files(root, files):
    changed = []
    for relative, content in files.items():
        path = Path(root) / relative
        old = path.read_bytes().decode("utf-8") if path.exists() else None
        if relative != README_PATH and old is not None:
            old = old.replace("\r\n", "\n")
        if old != content:
            path.parent.mkdir(parents=True, exist_ok=True)
            data = content if relative == README_PATH else content.replace("\n", "\r\n" if old and "\r\n" in old else "\n")
            path.write_text(data, encoding="utf-8", newline="")
            changed.append(relative)
    return changed


def pull_body(doc, changed, repo):
    site = f"https://{repo.split('/')[0].lower()}.github.io/{repo.split('/')[1]}"
    named = ", ".join(entry["login"] or entry["name"] for entry in doc["people"][:30])
    return "\n".join([
        PR_MARKER,
        f"Rebuilds the [Hall of fame]({site}/legacy.html) from the git history: `{DATA_PATH}`, "
        f"`{CONTRIBUTORS_PATH}` and the avatars in the README. `legacy.yml` (`tools/ci/legacy.py`) rewrites this pull "
        "request every day while the output differs from the default branch.",
        "",
        f"{len(doc['people'])} people, {len(doc['tools'])} tools, {doc['commits']} commits since {doc['since']}.",
        f"Changed: {', '.join('`' + path + '`' for path in changed)}.",
        "",
        f"On the wall: {named}{' and more' if len(doc['people']) > 30 else ''}.",
        "",
        "The files are generated: do not edit them here. A name that is missing is fixed in the commit trailers or "
        f"by `SINCE`/`TOOLS` in `tools/ci/legacy.py`. Do not push to `{BRANCH}`: the next run replaces it.",
        "",
    ])


class PublishError(RuntimeError):
    pass


def commit_files(git, parent, files, message):
    """A commit on parent that sets every path of files; nothing checked out is touched."""
    import tempfile
    with tempfile.TemporaryDirectory() as temp:
        index = {"GIT_INDEX_FILE": os.path.join(temp, "index")}
        git.run("read-tree", parent, env=index)
        for path, content in files.items():
            blob = git.out("hash-object", "-w", "--stdin", data=content.encode("utf-8"))
            git.run("update-index", "--add", "--cacheinfo", f"100644,{blob},{path}", env=index)
        tree = git.out("write-tree", env=index)
    name, email = BOT_IDENTITY
    identity = {"GIT_AUTHOR_NAME": name, "GIT_AUTHOR_EMAIL": email, "GIT_COMMITTER_NAME": name, "GIT_COMMITTER_EMAIL": email}
    return git.out("commit-tree", tree, "-p", parent, data=message.encode("utf-8"), env=identity)


def publish(github, git, repo, base, since=SINCE, branch=BRANCH, dry_run=False, log=print):
    """Brings the branch and its pull request in line with the history; returns a summary dict."""
    import ingest
    for _attempt in range(3):
        main_sha = git.fetch(base)
        if not main_sha:
            raise PublishError(f"the default branch {base} was not found")
        bot_sha = git.fetch(branch)
        doc = build_document(github, repo, since)
        readme = git.show(main_sha, README_PATH)
        files = outputs(doc, readme)
        if README_PATH not in files:
            log(f"::warning::{README_PATH} has no {START} markers: the Hall of fame avatars are not updated")
        changed = [path for path, content in files.items() if git.show(main_sha, path) != content]
        pull = ingest.find_pull(github, repo, branch, base)
        summary = {"people": len(doc["people"]), "tools": len(doc["tools"]), "commits": doc["commits"],
                   "changed": changed, "pull": pull["number"] if pull else None, "pushed": False}
        if not changed:
            if dry_run:
                log("[dry run] nothing to change" + (f"; would close #{pull['number']}" if pull else ""))
                return summary
            if pull:
                github.post(f"/repos/{repo}/issues/{pull['number']}/comments",
                            {"body": f"The Hall of fame on `{base}` is up to date. Closing this; the next change opens a new one."})
                github.patch(f"/repos/{repo}/pulls/{pull['number']}", {"state": "closed"})
                summary["closed"] = pull["number"]
            if bot_sha:
                git.delete(branch, bot_sha)
            return summary
        up_to_date = bot_sha and git.parents(bot_sha) == [main_sha] and all(git.show(bot_sha, p) == files[p] for p in changed)
        if not up_to_date:
            commit = commit_files(git, main_sha, {path: files[path] for path in changed},
                                  PULL_TITLE + "\n\nGenerated by tools/ci/legacy.py from the commits of the default "
                                  "branch (.github/workflows/legacy.yml).\n")
            if dry_run:
                log(f"[dry run] would push {commit[:10]} to {branch}: {', '.join(changed)}")
            elif not git.push(commit, branch, bot_sha):
                log(f"{branch} moved while this run worked; starting over")
                continue
            else:
                summary["pushed"] = True
        body = pull_body(doc, changed, repo)
        if dry_run:
            log(f"[dry run] would {'update #' + str(pull['number']) if pull else 'open'} the pull request:\n{body}")
            return summary
        if pull:
            if pull.get("title") != PULL_TITLE or pull.get("body") != body:
                github.patch(f"/repos/{repo}/pulls/{pull['number']}", {"title": PULL_TITLE, "body": body})
            return summary
        try:
            created = github.post(f"/repos/{repo}/pulls", {"title": PULL_TITLE, "head": branch, "base": base,
                                                           "body": body, "maintainer_can_modify": True})
        except github_api.GitHubError as error:
            if error.status == 422 and "already exists" in error.message:
                continue
            raise PublishError(f"could not open the pull request ({error}). Allow GitHub Actions to create pull "
                               "requests: Settings > Actions > General > Workflow permissions. The branch "
                               f"{branch} has the files; a maintainer can open the pull request by hand.") from None
        summary["pull"] = created["number"]
        return summary
    raise PublishError(f"{branch} kept moving; run the workflow again")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    for name, help_text in (("build", "write the three outputs in the working tree"),
                            ("publish", "update the bot/legacy branch and its pull request")):
        command = commands.add_parser(name, help=help_text)
        command.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
        command.add_argument("--since", default=SINCE, help=f"first day that counts (default {SINCE})")
        command.add_argument("--dry-run", action="store_true")
    commands.choices["build"].add_argument("--root", default=str(ROOT))
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", args.repo or ""):
        parser.error("--repo owner/name (or GITHUB_REPOSITORY) is required")
    dry_run = args.dry_run or os.environ.get("DRY_RUN", "").lower() == "true"
    try:
        github = github_api.GitHub(os.environ.get("GITHUB_TOKEN"))
        if args.command == "build":
            doc = build_document(github, args.repo, args.since)
            readme = Path(args.root, README_PATH)
            files = outputs(doc, readme.read_bytes().decode("utf-8") if readme.exists() else "")
            if dry_run:
                print(json.dumps({"people": len(doc["people"]), "tools": len(doc["tools"]), "commits": doc["commits"]}))
                return 0
            print(json.dumps({"written": write_files(args.root, files)}))
            return 0
        import ingest
        event = github_api.read_event()
        summary = publish(github, ingest.Git(), args.repo, ingest.default_branch(github, args.repo, event), args.since,
                          dry_run=dry_run)
        print(json.dumps(summary))
        step = os.environ.get("GITHUB_STEP_SUMMARY")
        if step:
            with open(step, "a", encoding="utf-8") as handle:
                handle.write(f"### Hall of fame\n{summary['people']} people, {summary['tools']} tools; "
                             f"changed: {', '.join(summary['changed']) or 'nothing'}"
                             + (f"; pull request #{summary['pull']}" if summary.get("pull") else "") + ".\n")
    except (github_api.GitHubError, PublishError) as error:
        print(f"::error::{error}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
