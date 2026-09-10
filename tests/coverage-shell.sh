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
TRACE=$(mktemp)
trap 'rm -f "$TRACE"' EXIT

# The tests source audioctl and call into it; tracing them traces it.
export AUDIOCTL_TRACE="$TRACE"
bash "$HERE/test-audioctl.sh" >/dev/null 2>&1 || true

if [ ! -s "$TRACE" ]; then
    echo "no trace collected - is test-audioctl.sh writing one?" >&2
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
    was_continued, continued = continued, line.rstrip().endswith("\\")
    # A command broken over several lines with a backslash is one statement.
    # bash announces it once, but not always at the line it starts on - so the
    # whole run of lines counts as one, and as reached if any of them was.
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
    # what runs inside the branch, never the label.
    if re.match(r"^[A-Za-z0-9_*?|.\-\[\]]+\)$", s):
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
