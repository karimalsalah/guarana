#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A small GitHub REST client for ingest.py and triage.py. Standard library only.

It sends the token only to the API host it was made for (GITHUB_API_URL, else api.github.com) and
follows pagination links only on that host.
"""

import json
import os
import re
import time
import urllib.error
import urllib.parse
import urllib.request

API = "https://api.github.com"
USER_AGENT = "guarana-ingest"


class GitHubError(Exception):
    def __init__(self, status, message, path):
        super().__init__(f"HTTP {status} from {path}: {message}")
        self.status = status
        self.message = message


def next_link(header):
    """The rel="next" URL of a Link header, or None."""
    for part in (header or "").split(","):
        match = re.match(r'\s*<([^>]+)>\s*;\s*rel="next"', part)
        if match:
            return match.group(1)
    return None


class GitHub:
    def __init__(self, token, api=None, opener=None, sleep=time.sleep):
        self.token = token
        self.api = (api or os.environ.get("GITHUB_API_URL") or API).rstrip("/")
        self.opener = opener or urllib.request.urlopen
        self.sleep = sleep

    def request(self, method, path, body=None, params=None, accept="application/vnd.github+json"):
        """(decoded JSON or None, Link header) of one request; GitHubError on an HTTP error."""
        url = path if path.startswith(self.api + "/") else self.api + path
        if not url.startswith(self.api + "/"):
            raise ValueError(f"refusing a URL outside {self.api}: {path}")
        if params:
            url += ("&" if "?" in url else "?") + urllib.parse.urlencode(params)
        data = None if body is None else json.dumps(body).encode("utf-8")
        headers = {"Accept": accept, "User-Agent": USER_AGENT, "X-GitHub-Api-Version": "2022-11-28"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        if data is not None:
            headers["Content-Type"] = "application/json"
        for attempt in range(3):
            request = urllib.request.Request(url, data=data, headers=headers, method=method)
            try:
                with self.opener(request, timeout=30) as response:
                    raw = response.read()
                    link = response.headers.get("Link", "") if response.headers else ""
                    return (json.loads(raw.decode("utf-8")) if raw else None), link
            except urllib.error.HTTPError as error:
                if error.code in (502, 503, 504) and attempt < 2:
                    self.sleep(2 * (attempt + 1))
                    continue
                try:
                    message = json.loads(error.read().decode("utf-8")).get("message", "")
                except (ValueError, AttributeError, OSError):
                    message = error.reason or ""
                raise GitHubError(error.code, message, path) from None
            except urllib.error.URLError:
                if attempt < 2:
                    self.sleep(2 * (attempt + 1))
                    continue
                raise
        raise AssertionError("unreachable")

    def get(self, path, **params):
        return self.request("GET", path, params=params or None)[0]

    def post(self, path, body):
        return self.request("POST", path, body=body)[0]

    def patch(self, path, body):
        return self.request("PATCH", path, body=body)[0]

    def paginate(self, path, **params):
        """Every item of a list endpoint, following its Link headers."""
        params.setdefault("per_page", 100)
        items, url, query = [], path, params
        while url:
            data, link = self.request("GET", url, params=query)
            items.extend(data or [])
            url, query = next_link(link), None
        return items


def upsert_comment(github, repo, number, marker, body, dry_run=False, log=print):
    """Writes body as the one comment on an issue that a bot wrote with marker; returns
    "created", "updated" or "unchanged"."""
    comments = github.paginate(f"/repos/{repo}/issues/{number}/comments")
    mine = next((comment for comment in comments
                 if (comment.get("user") or {}).get("type") == "Bot" and marker in (comment.get("body") or "")),
                None)
    if mine and mine.get("body") == body:
        return "unchanged"
    if dry_run:
        log(f"[dry run] would {'update' if mine else 'create'} the comment on #{number}:\n{body}")
        return "updated" if mine else "created"
    if mine:
        github.patch(f"/repos/{repo}/issues/comments/{mine['id']}", {"body": body})
        return "updated"
    github.post(f"/repos/{repo}/issues/{number}/comments", {"body": body})
    return "created"


def from_environment():
    """A client for GITHUB_TOKEN and the repository in GITHUB_REPOSITORY."""
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN") or ""
    repo = os.environ.get("GITHUB_REPOSITORY", "")
    return GitHub(token), repo


def read_event(path=None):
    """The webhook payload of the run (GITHUB_EVENT_PATH), or {}."""
    path = path or os.environ.get("GITHUB_EVENT_PATH")
    if not path or not os.path.isfile(path):
        return {}
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)
