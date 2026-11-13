#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Issue form parsing and text clean-up shared by ingest.py and triage.py.

GitHub writes an issue form's answers into the issue body as "### <label>" followed by the answer,
and "_No response_" for a field left empty. Standard library only.
"""

import html
import re
import unicodedata

NO_RESPONSE = "_No response_"

# The section labels of every form in .github/ISSUE_TEMPLATE/ (tests/test_forms.py keeps this list
# in step with the forms).
FORM_LABELS = (
    # device-report.yml
    "Before submitting", "Device", "Form factor", "CPU", "Graphics card", "GPU driver", "RAM",
    "Operating system", "Power source", "Power mode", "Emulator build", "Game", "Scene",
    "Settings and switches", "Results", "tools/bench rows", "Screenshot", "Anything else",
    # crash.yml, game_bug.yml, visual_bug.yml, ui_bug.yml
    "Regression", "Last known working build", "Steps to reproduce", "Log / crash dump",
    # agent-task.yml
    "Goal", "Files", "Acceptance test", "Measurement", "Verifiable without the game?", "Size",
    "Constraints and links",
    # perf-idea.yml
    "Hypothesis", "Where in the code", "How to measure", "Expected gain", "Evidence so far", "Risk",
    # feature_request.yml, blank.yml
    "What do you want ?", "Why it's useful", "Issue",
)

# "Graphics card" options of the forms: short name, vendor, integrated or discrete.
GPU_OPTIONS = {
    "Intel Arc 140T / 140V (Arrow Lake H, Lunar Lake)": ("Intel Arc 140T / 140V", "intel", "integrated"),
    "Intel Arc (Meteor Lake)": ("Intel Arc (Meteor Lake)", "intel", "integrated"),
    "Intel Arc (Panther Lake)": ("Intel Arc (Panther Lake)", "intel", "integrated"),
    "Other Intel integrated (Iris Xe, UHD)": ("Intel Iris Xe / UHD", "intel", "integrated"),
    "AMD Radeon 780M / 760M": ("AMD Radeon 780M / 760M", "amd", "integrated"),
    "AMD Radeon 880M / 890M": ("AMD Radeon 880M / 890M", "amd", "integrated"),
    "AMD Ryzen Z1 / Z2 (handheld)": ("AMD Ryzen Z1 / Z2", "amd", "integrated"),
    "Other AMD integrated": ("AMD integrated", "amd", "integrated"),
    "Intel Arc discrete (A or B series)": ("Intel Arc A / B series", "intel", "discrete"),
    "NVIDIA RTX 50 series": ("NVIDIA RTX 50 series", "nvidia", "discrete"),
    "NVIDIA RTX 40 series": ("NVIDIA RTX 40 series", "nvidia", "discrete"),
    "NVIDIA RTX 30 series or older": ("NVIDIA RTX 30 series or older", "nvidia", "discrete"),
    "AMD RX 9000 series": ("AMD RX 9000 series", "amd", "discrete"),
    "AMD RX 7000 series": ("AMD RX 7000 series", "amd", "discrete"),
    "AMD RX 6000 series or older": ("AMD RX 6000 series or older", "amd", "discrete"),
    "Other": ("Other", "other", "unknown"),
    # perf-idea.yml only
    "Any GPU": ("Any GPU", "other", "unknown"),
    "NVIDIA": ("NVIDIA", "nvidia", "discrete"),
    "AMD discrete": ("AMD discrete", "amd", "discrete"),
}


def gpu_labels(option):
    """The hardware labels for a "Graphics card" answer, by the same tests as label-issues.yml."""
    labels = set()
    if re.search(r"(?i)intel", option) and re.search(r"(?i)arc", option):
        labels.add("intel-arc")
    if re.search(r"(?i)amd", option) and re.search(r"(?i)integrated|[0-9]{3}M\b|Z[12]\b", option):
        labels.add("amd-igpu")
    if re.search(r"(?i)handheld", option):
        labels.add("handheld")
    return labels


# ---------------------------------------------------------------------------------------------
# Sections

_HEADING = re.compile(r"^###[ \t]+(.+?)[ \t]*$")
_CHECKBOX = re.compile(r"^\s*[-*]\s*\[([ xX])\]\s*(.+?)\s*$")
_FENCE = re.compile(r"^[ \t]{0,3}(`{3,}|~{3,})[ \t]*([^`\s]*)[^\n]*\n(.*?)(?:^[ \t]{0,3}\1[`~]*[ \t]*$|\Z)", re.S | re.M)


def normalize_newlines(text):
    return (text or "").replace("\r\n", "\n").replace("\r", "\n")


def parse_form(body, labels=FORM_LABELS):
    """{label: answer} of an issue body written by an issue form.

    Only a line that is exactly "### <known label>" starts a section, and only the first time that
    label appears, so an answer that contains its own "###" lines stays whole. Empty answers and
    "_No response_" are left out.
    """
    known = {label.casefold(): label for label in labels}
    sections, seen, current, lines = {}, set(), None, []

    def close():
        if current is None:
            return
        text = "\n".join(lines).strip()
        if text and text != NO_RESPONSE:
            sections[current] = text

    for line in normalize_newlines(body).split("\n"):
        match = _HEADING.match(line)
        label = known.get(match.group(1).casefold()) if match else None
        if label and label not in seen:
            close()
            current, lines = label, []
            seen.add(label)
        elif current is not None:
            lines.append(line)
    close()
    return sections


def form_headings(body, labels=FORM_LABELS):
    """The known section labels whose "### <label>" heading is in the body, answered or not."""
    known = {label.casefold(): label for label in labels}
    return {known[match.group(1).casefold()] for match in map(_HEADING.match, normalize_newlines(body).split("\n"))
            if match and match.group(1).casefold() in known}


def parse_checkboxes(answer):
    """{option text, case-folded: ticked} of a checkboxes answer."""
    boxes = {}
    for line in normalize_newlines(answer).split("\n"):
        match = _CHECKBOX.match(line)
        if match:
            boxes[match.group(2).casefold()] = match.group(1) != " "
    return boxes


def fenced_blocks(text):
    """[(info string, content)] of the fenced code blocks in markdown text; an unclosed fence runs
    to the end."""
    return [(match.group(2).lower(), match.group(3)) for match in _FENCE.finditer(normalize_newlines(text))]


def without_code(text):
    """Markdown text without its fenced blocks and inline code spans."""
    text = _FENCE.sub("\n", normalize_newlines(text))
    return re.sub(r"`[^`\n]*`", " ", text)


# ---------------------------------------------------------------------------------------------
# Text clean-up

_INVISIBLE = re.compile("[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f-\u009f­؜᠎"
                        "​-‏ -‮⁠-⁩﻿￹-￻]")
_HTML_COMMENT = re.compile(r"<!--.*?(?:-->|\Z)", re.S)
_HTML_IMG = re.compile(r"(?i)<img\b[^>]*>")
_HTML_TAG = re.compile(r"</?[A-Za-z][A-Za-z0-9-]*(?:\s[^<>]*)?/?>")
_MD_IMAGE = re.compile(r"!\[[^\]\n]*\]\([^)\n]*\)")
_MD_LINK = re.compile(r"\[([^\]\n]*)\]\([^)\n]*\)")
_FENCE_LINE = re.compile(r"^[ \t]*(?:`{3,}|~{3,}).*$", re.M)
_EMPHASIS = re.compile(r"\*\*|__|~~")
_LINE_MARKUP = re.compile(r"^[ \t]{0,3}(?:#{1,6}[ \t]+|>[ \t]?|[-*+][ \t]+(?:\[[ xX]\][ \t]+)?|\d{1,3}[.)][ \t]+)", re.M)
_SPACES = re.compile(r"[ \t ]+")


def normalize_text(text):
    """NFKC, LF line ends, no control, zero-width or bidirectional formatting characters."""
    text = unicodedata.normalize("NFKC", normalize_newlines(text))
    return _INVISIBLE.sub("", text)


def strip_markup(text):
    """Plain text of a markdown answer: no HTML, images, link targets, fences or emphasis."""
    text = _HTML_COMMENT.sub("", text)
    text = _MD_IMAGE.sub("", text)
    text = _HTML_IMG.sub("", text)
    text = _HTML_TAG.sub("", text)
    text = _MD_LINK.sub(r"\1", text)
    text = _FENCE_LINE.sub("", text)
    text = text.replace("`", "")
    text = _EMPHASIS.sub("", text)
    text = _LINE_MARKUP.sub("", text)
    return html.unescape(text)


# Personal data. Serial numbers and ids are recognised by what precedes them, never by their shape,
# so model numbers (UX5406), driver versions (32.0.101.9175), memory (LPDDR5X-8400) and commit
# hashes (1a2b3c4) stay.
_PATH_END = r"[^\s\"'<>|,;]"
_URL = re.compile(r"(?i)\b(?:https?://|www\.)[^\s<>\"')\]]*[^\s<>\"')\],.;:!?]")
_EMAIL = re.compile(r"(?i)\b[a-z0-9._%+-]+@[a-z0-9-]+(?:\.[a-z0-9-]+)*\.[a-z]{2,}\b")
_WINDOWS_PATH = re.compile(r"(?i)(?<![a-z0-9])[a-z]:[\\/]" + _PATH_END + "*")
_BACKSLASH_PATH = re.compile(_PATH_END + r"*\\" + _PATH_END + "+")
_HOME_PATH = re.compile(r"(?:~(?=/)|(?<![\w.])/(?:home|Users|var/home|run/media|media|mnt)/[^/\s\"'<>|,;]+)"
                        r"(?:/" + _PATH_END + "*)?")
_UNIX_PATH = re.compile(r"(?<![\w.:/~<])/(?:[\w.+-]+/)+[\w.+-]*")
_MAC = re.compile(r"(?i)\b[0-9a-f]{2}(?:[:-][0-9a-f]{2}){5}\b")
_OCTET = r"(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)"
_IPV4 = re.compile(r"(?<![\d.])" + _OCTET + r"(?:\." + _OCTET + r"){3}(?![\d.])")
_IPV6 = re.compile(r"(?i)(?<![0-9a-f:])(?:[0-9a-f]{1,4}:){7}[0-9a-f]{1,4}(?![0-9a-f:])")
_HOST = re.compile(r"(?i)\b(?:desktop|laptop)-(?=[a-z]*\d)[a-z0-9]{5,15}\b")
_SERIAL = re.compile(r"(?i)\b(?:s/n|sn|serial(?:[ \t]*(?:no\.?|number|num|#))?|service[ \t]*tag|asset[ \t]*tag|imei|"
                     r"express[ \t]*service[ \t]*code)(?![a-z0-9])[ \t]*(?:is[ \t]+|[:#=-][ \t]*)?[a-z0-9][a-z0-9-]{3,}")
_ACCOUNT_ID = re.compile(r"(?i)\b(?:device|console|account|user|nsa|profile)[ _-]?id(?![a-z0-9])[ \t]*"
                         r"(?:is[ \t]+|[:#=-][ \t]*)?[a-z0-9][a-z0-9-]{5,}")
_UUID = re.compile(r"(?i)\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b")
_MENTION = re.compile(r"(?<![\w@/.<])@[A-Za-z][A-Za-z0-9-]{0,38}(?![\w-])")
# Words a reporter's login may also be; such a login is not scrubbed from the text.
_COMMON_WORDS = {"amd", "arc", "ally", "asus", "claw", "deck", "desktop", "game", "games", "gpu", "guarana",
                 "handheld", "intel", "laptop", "legion", "lenovo", "lichii", "linux", "mesa", "msi",
                 "nvidia", "radeon", "rog", "steam", "windows", "xbox", "zelda"}


def scrub(text, logins=()):
    """text without links, e-mail and network addresses, folder paths, host names, serial numbers,
    account ids, @mentions and the given GitHub logins."""
    text = _URL.sub("<link>", text)
    text = _EMAIL.sub("<email>", text)
    for pattern in (_WINDOWS_PATH, _BACKSLASH_PATH, _HOME_PATH, _UNIX_PATH):
        text = pattern.sub("<path>", text)
    text = _MAC.sub("<mac>", text)
    text = _IPV6.sub("<ip>", text)
    text = _IPV4.sub("<ip>", text)
    text = _HOST.sub("<host>", text)
    text = _SERIAL.sub("<serial>", text)
    text = _ACCOUNT_ID.sub("<id>", text)
    text = _UUID.sub("<id>", text)
    text = _MENTION.sub("<user>", text)
    for login in logins:
        if login and len(login) >= 3 and login.casefold() not in _COMMON_WORDS:
            text = re.sub(r"(?i)(?<![A-Za-z0-9])" + re.escape(login) + r"(?![A-Za-z0-9])", "<user>", text)
    return text


def truncate(text, limit):
    if len(text) <= limit:
        return text
    cut = text[:limit - 1]
    space = cut.rfind(" ")
    if space > limit * 0.6:
        cut = cut[:space]
    return cut.rstrip(" ,;:.-") + "…"


def one_line(text, limit, logins=()):
    """A form answer as one line of plain, scrubbed text of at most limit characters."""
    text = scrub(strip_markup(normalize_text(text)), logins)
    out = ""
    for line in text.split("\n"):
        part = _SPACES.sub(" ", line).strip()
        if not part:
            continue
        if not out:
            out = part
        elif out[-1] in ".;:,!?":
            out += " " + part
        else:
            out += "; " + part
    return truncate(out, limit)


def md_escape(text):
    """text for a markdown comment: no formatting, HTML, table breaks or @mentions."""
    text = str(text).replace("\\", "\\\\")
    for char in "`*_[]<>|#~":
        text = text.replace(char, "\\" + char)
    return text.replace("@", "@​").replace("\n", " ")
