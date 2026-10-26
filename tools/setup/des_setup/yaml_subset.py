"""A small reader for the YAML that RPCS3 writes (docs/44 8.6, docs/90 3.1):
config.yml, the custom configs, patch_config.yml, games.yml and vfs.yml.

It knows the subset those files use: block mappings by indent ("key: value"
and "key:" with the value on the lines below), plain, single-quoted and
double-quoted scalars and keys, comments, and block sequences ("- item").
Every scalar comes back as a string; the caller decides what it means. A flow
value such as "[]" or "{}" comes back as that text. A line it cannot read is
skipped, never guessed at, so a stranger file gives fewer keys, not wrong ones.
"""


def load(text: str) -> dict:
    """The file's top mapping, as nested dicts (and lists of strings)."""
    root = {}
    stack = [(-1, root)]               # (the indent of the lines inside, the container)
    want = None                        # (dict, key, indent): a key whose value is on the lines below
    for raw in text.lstrip("﻿").splitlines():
        body = raw.strip()
        if not body or body.startswith("#") or body in ("---", "..."):
            continue
        indent = len(raw) - len(raw.lstrip(" "))
        item = body.startswith("- ") or body == "-"
        if want is not None:
            parent, key, at = want
            want = None
            if indent > at or (indent == at and item):
                if item:
                    parent[key] = []
                elif split_key(body) is not None:
                    parent[key] = {}
                else:                  # a scalar on a line of its own, such as "[]" under "Libraries Control:"
                    parent[key] = scalar(body)
                    continue
                stack.append((indent, parent[key]))
        # Back out of the containers this line is not inside (a list at a key's own indent ends at the next key).
        while len(stack) > 1 and (indent < stack[-1][0] or
                                  (indent == stack[-1][0] and isinstance(stack[-1][1], list) and not item)):
            stack.pop()
        here = stack[-1][1]
        if item:
            if isinstance(here, list):
                here.append(scalar(body[1:].strip()))
            continue
        kv = split_key(body)
        if kv is None or not isinstance(here, dict):
            continue
        key, value = kv
        if value:
            here[key] = scalar(value)
        else:
            here[key] = ""
            want = (here, key, indent)
    return root


def load_file(path) -> dict:
    """load() of a file, or {} when it is missing or unreadable. It is only read."""
    try:
        with open(path, "rb") as f:
            return load(f.read().decode("utf-8", "replace"))
    except OSError:
        return {}


def split_key(body: str):
    """(key, the rest of the line after "key:"), or None when the line is not a mapping entry."""
    if body[:1] in ("'", '"'):
        key, end = _quoted(body)
        if end is None:
            return None
        rest = body[end:].lstrip()
        if not rest.startswith(":") or (len(rest) > 1 and rest[1] != " "):
            return None
        return key, rest[1:].strip()
    if body[:1] in "{[?&*!|>%@`":    # a flow mapping, a complex key, an anchor or a tag: not in RPCS3's files
        return None
    at = body.find(": ")
    if at < 0:
        return (body[:-1].rstrip(), "") if body.endswith(":") and len(body) > 1 else None
    return body[:at].rstrip(), body[at + 2:].strip()


def scalar(s: str) -> str:
    """A value's text: quotes undone, a plain value's trailing comment dropped."""
    s = s.strip()
    if s[:1] in ("'", '"'):
        value, end = _quoted(s)
        if end is not None:
            return value
    at = s.find(" #")
    return (s[:at] if at >= 0 else s).rstrip()


_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "0": "\0", "\\": "\\", '"': '"', "/": "/", " ": " ", "a": "\a",
            "b": "\b", "e": "\x1b", "f": "\f", "v": "\v"}


def _quoted(s: str):
    """The quoted text at the start of s, and the index after its closing quote (None when it has none)."""
    q, out, i = s[0], [], 1
    while i < len(s):
        c = s[i]
        if q == "'" and c == "'":
            if s[i + 1:i + 2] == "'":
                out.append("'")
                i += 2
                continue
            return "".join(out), i + 1
        if q == '"' and c == '"':
            return "".join(out), i + 1
        if q == '"' and c == "\\" and i + 1 < len(s):
            e = s[i + 1]
            width = {"x": 2, "u": 4, "U": 8}.get(e)
            if width:
                try:
                    out.append(chr(int(s[i + 2:i + 2 + width], 16)))
                except ValueError:
                    out.append(s[i + 2:i + 2 + width])
                i += 2 + width
                continue
            out.append(_ESCAPES.get(e, e))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out), None
