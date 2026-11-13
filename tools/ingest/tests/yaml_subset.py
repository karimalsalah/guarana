#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A strict reader for the YAML subset that this repository's workflows and issue forms use.

Block mappings and sequences, plain, single- and double-quoted scalars, literal and folded block
scalars, one-line flow sequences and empty flow mappings. Anything else (anchors, aliases, tags,
multi-line plain or quoted scalars, flow mappings with content, tabs in indentation, duplicate keys,
": " inside a plain scalar) is an error, so a file it reads is YAML with the structure it returns.
Scalars stay strings and an empty value is None. The tests use it because the standard library has
no YAML parser; it is not a general one.
"""

import re

_KEY = re.compile(r"([A-Za-z0-9_][\w./-]*):(?: |$)")
_BLOCK = re.compile(r"([|>])([+-]?)[ \t]*(?:#.*)?$")
_ESCAPES = {"0": "\0", "a": "\a", "b": "\b", "t": "\t", "n": "\n", "v": "\v", "f": "\f", "r": "\r",
            "e": "\x1b", " ": " ", '"': '"', "/": "/", "\\": "\\", "N": "\x85", "_": "\xa0"}


class YamlError(ValueError):
    pass


def load(text):
    return _Parser(text).document()


class _Parser:
    def __init__(self, text):
        self.lines = text.replace("\r\n", "\n").split("\n")
        self.i = 0

    def error(self, message):
        raise YamlError(f"line {self.i + 1}: {message}")

    @staticmethod
    def _blank(line):
        stripped = line.strip()
        return not stripped or stripped.startswith("#")

    def _indent(self, line):
        width = len(line) - len(line.lstrip(" "))
        if line[width:width + 1] == "\t":
            self.error("tab in indentation")
        return width

    def _skip(self):
        while self.i < len(self.lines) and self._blank(self.lines[self.i]):
            self.i += 1

    @staticmethod
    def _is_item(content):
        return content == "-" or content.startswith("- ")

    def document(self):
        self._skip()
        if self.i < len(self.lines) and self.lines[self.i].strip() == "---":
            self.i += 1
            self._skip()
        if self.i >= len(self.lines):
            return None
        value = self._node(self._indent(self.lines[self.i]))
        self._skip()
        if self.i < len(self.lines):
            self.error("unexpected content after the document")
        return value

    def _node(self, indent):
        content = self.lines[self.i][indent:]
        return self._sequence(indent) if self._is_item(content) else self._mapping(indent)

    def _sequence(self, indent):
        items = []
        while True:
            self._skip()
            if self.i >= len(self.lines):
                return items
            line = self.lines[self.i]
            width = self._indent(line)
            if width < indent or not self._is_item(line[width:]):
                if width > indent:
                    self.error("bad indentation in a sequence")
                return items
            if width > indent:
                self.error("bad indentation in a sequence")
            after = line[width + 1:]
            column = width + 1 + (len(after) - len(after.lstrip(" ")))
            rest = after.strip(" ")
            if not rest or rest.startswith("#"):
                self.i += 1
                self._skip()
                if self.i < len(self.lines) and self._indent(self.lines[self.i]) > indent:
                    items.append(self._node(self._indent(self.lines[self.i])))
                else:
                    items.append(None)
            elif self._is_item(rest):
                self.error("nested sequences on one line are not supported")
            elif _KEY.match(rest) or rest[0] in "\"'" and self._quoted_key(rest):
                self.lines[self.i] = " " * column + rest  # a mapping that starts on the item's line
                items.append(self._mapping(column))
            elif _BLOCK.match(rest):
                items.append(self._block_scalar(rest, indent))
            else:
                items.append(self._scalar(rest))
                self.i += 1

    def _quoted_key(self, content):
        try:
            _value, end = self._quoted(content)
        except YamlError:
            return None
        rest = content[end:]
        return rest[:1] == ":" and (len(rest) == 1 or rest[1] == " ")

    def _mapping(self, indent):
        result = {}
        while True:
            self._skip()
            if self.i >= len(self.lines):
                return result
            line = self.lines[self.i]
            width = self._indent(line)
            if width < indent:
                return result
            if width > indent:
                self.error("bad indentation in a mapping")
            content = line[width:]
            if self._is_item(content):
                return result
            if content[0] in "\"'" and self._quoted_key(content):
                key, end = self._quoted(content)
                rest = content[end + 1:]
            else:
                match = _KEY.match(content)
                if not match:
                    self.error(f"expected 'key: value', got {content!r}")
                key, rest = match.group(1), content[match.end():]
            if key in result:
                self.error(f"duplicate key {key!r}")
            rest = rest.strip()
            if not rest or rest.startswith("#"):
                self.i += 1
                self._skip()
                result[key] = None
                if self.i < len(self.lines):
                    following = self.lines[self.i]
                    width_next = self._indent(following)
                    if width_next > indent:
                        result[key] = self._node(width_next)
                    elif width_next == indent and self._is_item(following[width_next:]):
                        result[key] = self._sequence(width_next)
            elif _BLOCK.match(rest):
                result[key] = self._block_scalar(rest, indent)
            else:
                result[key] = self._scalar(rest)
                self.i += 1

    def _block_scalar(self, header, parent_indent):
        style, chomp = _BLOCK.match(header).groups()
        self.i += 1
        block_indent, lines = None, []
        while self.i < len(self.lines):
            line = self.lines[self.i]
            if not line.strip():
                lines.append("")
                self.i += 1
                continue
            width = len(line) - len(line.lstrip(" "))
            if block_indent is None:
                if width <= parent_indent:
                    break
                block_indent = width
            if width < block_indent:
                if width > parent_indent:
                    self.error("bad indentation in a block scalar")
                break
            lines.append(line[block_indent:])
            self.i += 1
        trailing = 0
        while lines and lines[-1] == "":
            lines.pop()
            trailing += 1
        if style == "|":
            text = "\n".join(lines)
        else:
            parts, paragraph = [], []
            for line in lines:
                if line == "" or line.startswith(" "):
                    if paragraph:
                        parts.append(" ".join(paragraph))
                        paragraph = []
                    parts.append(line)
                else:
                    paragraph.append(line)
            if paragraph:
                parts.append(" ".join(paragraph))
            text = "\n".join(parts)
        if chomp == "-" or not text:
            return text
        return text + "\n" * (1 + trailing if chomp == "+" else 1)

    def _quoted(self, text):
        """(value, index after the closing quote) of a quoted scalar at the start of text."""
        quote, out, index = text[0], [], 1
        while index < len(text):
            char = text[index]
            if quote == "'" and char == "'":
                if text[index + 1:index + 2] == "'":
                    out.append("'")
                    index += 2
                    continue
                return "".join(out), index + 1
            if quote == '"' and char == '"':
                return "".join(out), index + 1
            if quote == '"' and char == "\\":
                code = text[index + 1:index + 2]
                if code in _ESCAPES:
                    out.append(_ESCAPES[code])
                    index += 2
                    continue
                if code in ("x", "u", "U"):
                    size = {"x": 2, "u": 4, "U": 8}[code]
                    digits = text[index + 2:index + 2 + size]
                    if len(digits) != size or not re.fullmatch(r"[0-9A-Fa-f]+", digits):
                        self.error("bad escape in a double-quoted scalar")
                    out.append(chr(int(digits, 16)))
                    index += 2 + size
                    continue
                self.error(f"unknown escape \\{code}")
            out.append(char)
            index += 1
        self.error("quoted scalars must end on their line")

    def _flow_sequence(self, text):
        items, index, item = [], 1, ""
        while index < len(text):
            char = text[index]
            if char in "\"'" and not item.strip():
                value, end = self._quoted(text[index:])
                items.append(value)
                index += end
                tail = re.match(r"[ \t]*([,\]])", text[index:])
                if not tail:
                    self.error("expected ',' or ']' after a quoted item")
                index += tail.end()
                if tail.group(1) == "]":
                    return items, index
                continue
            if char in "[{":
                self.error("nested flow collections are not supported")
            if char in ",]":
                value = item.strip()
                if value:
                    if ": " in value or value.startswith(("&", "*", "!", "%", "@", "`")):
                        self.error(f"unsupported flow item {value!r}")
                    items.append(value)
                elif char == "," or items:
                    self.error("empty item in a flow sequence")
                item = ""
                index += 1
                if char == "]":
                    return items, index
                continue
            item += char
            index += 1
        self.error("flow sequences must end on their line")

    def _scalar(self, text):
        first = text[0]
        if first in "\"'":
            value, end = self._quoted(text)
        elif first == "[":
            value, end = self._flow_sequence(text)
        elif first == "{":
            match = re.match(r"\{[ \t]*\}", text)
            if not match:
                self.error("flow mappings with content are not supported")
            value, end = {}, match.end()
        elif first in "&*!%@`|>?,]}#":
            self.error(f"unsupported or reserved start of a scalar: {first!r}")
        else:
            value = re.split(r"[ \t]#", text, maxsplit=1)[0].rstrip()
            if ": " in value or value.endswith(":"):
                self.error(f"': ' in a plain scalar (quote it): {value!r}")
            if value.startswith("- "):
                self.error("'- ' at the start of a plain scalar")
            return value
        tail = text[end:].strip()
        if tail and not tail.startswith("#"):
            self.error(f"text after a quoted or flow scalar: {tail!r}")
        return value
