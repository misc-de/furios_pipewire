#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# Line coverage for the shell, without a tool for it.
#
# bash has no coverage support, but it will tell you which line it is about to
# run: PS4 carries LINENO, set -x prints it. Collecting those and comparing
# them against the lines that have code on them is the same accounting gcov
# does, done by hand.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
TARGET=${1:-$ROOT/audioctl}
TEST=${2:-$HERE/test-audioctl.sh}
TRACE=$(mktemp)
trap 'rm -f "$TRACE"' EXIT

# The tests source audioctl and call into it; tracing them traces it.
export AUDIOCTL_TRACE="$TRACE"
bash "$TEST" >/dev/null 2>&1 || true

if [ ! -s "$TRACE" ]; then
    echo "no trace collected - is $(basename "$TEST") writing one?" >&2
    exit 1
fi

python3 - "$TARGET" "$TRACE" <<'PY'
import re, sys

target, trace = sys.argv[1], sys.argv[2]
name = target.rsplit("/", 1)[-1]

hit = set()
for line in open(trace, errors="replace"):
    m = re.match(r"\+*\s*([^:]+):(\d+):", line)
    if m and m.group(1).rsplit("/", 1)[-1] == name:
        hit.add(int(m.group(2)))

# Lines with code on them. Comments, blank lines and the lone closing brace of
# a function are not statements bash announces - and neither is the body of a
# here-document, which is data on its way to a command rather than anything
# that runs. Counting those put a floor of twenty-odd lines under this file
# that no test could ever lift.
executable, missing = set(), []
heredoc_end = None
continued = False
statement_start = 0
for n, line in enumerate(open(target, errors="replace"), 1):
    s = line.strip()
    stripped = line.rstrip()
    # A backslash continues a command, and so does a trailing pipe or a
    # trailing && / || - bash announces the whole pipeline once, at its LAST
    # line, so without this the first lines of one look unreached.
    was_continued, continued = continued, (
        stripped.endswith("\\") or stripped.endswith("|")
        or stripped.endswith("&&") or stripped.endswith("||"))
    # A command broken over several lines is one statement. bash announces it
    # once, but not always at the line it starts on - so the whole run of lines
    # counts as one, and as reached if any of them was.
    if was_continued:
        if n in hit:
            hit.add(statement_start)
        continue
    statement_start = n
    if heredoc_end is not None:
        if s == heredoc_end:
            heredoc_end = None
        continue
    m = re.search(r"<<-?\s*'?([A-Za-z_][A-Za-z0-9_]*)'?", line)
    if m:
        heredoc_end = m.group(1)
    if (not s or s.startswith("#") or s in ("}", "fi", "done", "esac", "else", ";;", "fi ;;", "done ;;", "esac ;;")
            or s.startswith("}") and len(s) < 3):
        continue
    if re.match(r"^[a-z_]+\(\)\s*\{?$", s) or s == "{":
        continue
    # The pattern line of a case branch is not a command either - bash reports
    # what runs inside the branch, never the label. Two shapes: a label on its
    # own, and a label whose branch is empty ("...) ;;"), which is how a case
    # says "this one is fine, do nothing". Patterns can contain quotes, slashes
    # and variables, so match on the shape rather than on the character set -
    # an empty branch used to count as a line no test could ever reach.
    if re.match(r"^[A-Za-z0-9_*?|.\-\[\]]+\)$", s) or re.match(r"^\S.*\)\s*;;$", s):
        continue
    executable.add(n)

for n in sorted(executable):
    if n not in hit:
        missing.append(n)

total = len(executable)
covered = total - len(missing)
print("  %s: %.2f%% of %d lines" % (name, covered / total * 100 if total else 100, total))
if missing and len(sys.argv) > 3 or __import__("os").environ.get("SHOW_MISSING"):
    print("      not reached:", " ".join(str(n) for n in missing[:60]))
PY
