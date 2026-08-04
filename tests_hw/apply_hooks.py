#!/usr/bin/env python3
"""
Applies the two trace hooks to src/util/active_object/active_object.c.

Use this instead of `patch` when the working copy has CRLF line endings, which
makes patch reject every hunk with "different line endings".

    python3 tests_hw/apply_hooks.py            # apply
    python3 tests_hw/apply_hooks.py --revert   # undo
    python3 tests_hw/apply_hooks.py --check    # report status only

Run it from the repository root. It preserves whatever line endings the file
already has, and it is safe to run twice: applying an already patched file or
reverting an already clean file both report "no change needed" instead of
corrupting anything.
"""

import sys
from pathlib import Path

TARGET = Path("src/util/active_object/active_object.c")

INCLUDE_ANCHOR = "#include <stddef.h>"
INCLUDE_BLOCK = [
    "",
    "/* User libraries */",
    '#include "util/trace/util_trace.h"',
]

DISPATCH_ANCHOR = "        (void)util_fsm_send_event(&active_object->active_fsm, event);"
DISPATCH_BLOCK = [
    "        util_trace_ao_dispatch(active_object, ((Event*)event)->type);",
    "",
]

# The post must be recorded BEFORE the push, otherwise the AO thread can
# dispatch the event before the post is recorded and the pairing desynchronizes.
POST_PUSH_LINE = "    eStatus status = util_queue_push(&active_object->event_queue, event);"
POST_BEFORE_BLOCK = [
    "    util_trace_ao_post(active_object, event->type);",
    "",
]
POST_FAIL_ANCHOR = "        return eSTATUS_ACTION_FAILED;"
POST_FAIL_BLOCK = [
    "        util_trace_ao_post_cancel(active_object);",
]

MARKERS = (
    "util_trace_ao_post",
    "util_trace_ao_post_cancel",
    "util_trace_ao_dispatch",
    "util_trace.h",
)


def read(path):
    """Returns (lines_without_endings, line_ending_string)."""
    raw = path.read_bytes().decode("utf-8")
    if "\r\n" in raw:
        ending = "\r\n"
    elif "\r" in raw:
        ending = "\r"
    else:
        ending = "\n"
    return raw.replace("\r\n", "\n").replace("\r", "\n").split("\n"), ending


def write(path, lines, ending):
    path.write_bytes(ending.join(lines).encode("utf-8"))


def is_applied(lines):
    return any(any(m in line for m in MARKERS) for line in lines)


def insert_after(lines, anchor, block, occurrence=1):
    seen = 0
    for i, line in enumerate(lines):
        if line == anchor:
            seen += 1
            if seen == occurrence:
                return lines[: i + 1] + block + lines[i + 1 :]
    raise LookupError(f"anchor not found: {anchor.strip()!r}")


def insert_before(lines, anchor, block, occurrence=1):
    seen = 0
    for i, line in enumerate(lines):
        if line == anchor:
            seen += 1
            if seen == occurrence:
                return lines[:i] + block + lines[i:]
    raise LookupError(f"anchor not found: {anchor.strip()!r}")


def insert_before_within(lines, scope_anchor, anchor, block):
    """Inserts before the first `anchor` that appears AFTER `scope_anchor`."""
    start = None
    for i, line in enumerate(lines):
        if line == scope_anchor:
            start = i
            break
    if start is None:
        raise LookupError(f"anchor not found: {scope_anchor.strip()!r}")

    for i in range(start, len(lines)):
        if lines[i] == anchor:
            return lines[:i] + block + lines[i:]

    raise LookupError(f"anchor not found after the queue push: {anchor.strip()!r}")


def apply(lines):
    lines = insert_after(lines, INCLUDE_ANCHOR, INCLUDE_BLOCK)
    lines = insert_before(lines, DISPATCH_ANCHOR, DISPATCH_BLOCK)
    # Record the post before the push, and undo it if the push fails.
    lines = insert_before(lines, POST_PUSH_LINE, POST_BEFORE_BLOCK)
    lines = insert_before_within(
        lines, POST_PUSH_LINE, POST_FAIL_ANCHOR, POST_FAIL_BLOCK
    )
    return lines


def revert(lines):
    out, skipped = [], 0
    for line in lines:
        if any(m in line for m in MARKERS) or line == "/* User libraries */":
            skipped += 1
            continue
        out.append(line)
    # Collapse the blank lines the removed blocks left behind.
    cleaned = []
    for i, line in enumerate(out):
        if line == "" and cleaned and cleaned[-1] == "" :
            continue
        cleaned.append(line)
    return cleaned, skipped


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "--apply"

    if not TARGET.exists():
        print(f"ERROR: {TARGET} not found. Run this from the repository root.")
        return 1

    lines, ending = read(TARGET)
    applied = is_applied(lines)
    name = {"\r\n": "CRLF", "\n": "LF", "\r": "CR"}[ending]

    if mode == "--check":
        print(f"file          {TARGET}")
        print(f"line endings  {name}")
        print(f"hooks         {'applied' if applied else 'not applied'}")
        return 0

    if mode == "--revert":
        if not applied:
            print("Hooks are not applied, no change needed.")
            return 0
        lines, removed = revert(lines)
        write(TARGET, lines, ending)
        print(f"Reverted, removed {removed} lines. Line endings preserved as {name}.")
        return 0

    if applied:
        print("Hooks are already applied, no change needed.")
        return 0

    try:
        lines = apply(lines)
    except LookupError as error:
        print(f"ERROR: {error}")
        print("The file does not look like the acoustic branch version.")
        print("Check with: git status && git branch --show-current")
        return 1

    write(TARGET, lines, ending)
    print(f"Applied 2 hooks and 1 include to {TARGET}.")
    print(f"Line endings preserved as {name}.")
    print("Verify with: git diff")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
