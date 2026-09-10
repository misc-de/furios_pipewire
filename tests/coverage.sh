#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# How much of the plugin the tests actually reach.
#
# A number nobody measures drifts. This compiles the C test a second time with
# gcov instrumentation - separately from the normal build, so the shipped
# plugin is never a coverage build - runs it, and prints line coverage per
# function.
#
# The number will stay modest, and that is the honest state of things: most of
# droid-device.c talks to PipeWire or the HAL, and exercising that needs a
# running graph, not a test binary. What can be covered are the decisions.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/src/pulseaudio-modules-droid-modern/src/common"
OUT=${OUT:-$(mktemp -d)}

command -v gcov >/dev/null || { echo "gcov not installed" >&2; exit 1; }
[ -f "$ROOT/poc/spa-droid/build/libdroid-common.a" ] || {
    echo "build the plugin first: ninja -C poc/spa-droid/build" >&2; exit 1; }

cc -I"$ROOT/poc/spa-droid" -I"$ROOT/poc/spa-droid/compat" \
   -I"$SRC" -I"$SRC/include" -I/usr/include/android -I/usr/include/spa-0.2 \
   -std=gnu11 -O0 -g --coverage \
   -DANDROID_VERSION_MAJOR=11 -DANDROID_VERSION_MINOR=0 -DANDROID_VERSION_PATCH=0 \
   -DHAVE_CONFIG_H -Wno-attributes -Wno-int-conversion -Wno-unused-parameter \
   -DTEST_FIXTURE="\"$ROOT/poc/spa-droid/tests/audio-policy-fixture.xml\"" \
   "$ROOT/poc/spa-droid/tests/test-droid-device.c" \
   -L"$ROOT/poc/spa-droid/build" -l:libdroid-common.a \
   -lexpat -lhybris-common -lhardware -lpthread -lm \
   -o "$OUT/test-cov"

( cd "$OUT" && ./test-cov >/dev/null )

echo
echo "== droid-device.c, line coverage per function =="
( cd "$OUT" && gcov -f -n ./*.gcno 2>/dev/null ) \
  | awk '/^Function/{fn=$2} /^Lines executed/{print $0, fn}' \
  | grep -E "'(route_priority|route_description|default_route|apply_route_props|build_route_body|build_profile|collect_routes|set_route|set_profile|impl_[a-z_]+|emit_[a-z_]+|params_changed|mix_port_of)'" \
  | sed "s/Lines executed://; s/of //" \
  | sort -t: -k1 -rn \
  | awk -F"[ %]" '{printf "  %7s  %s\n", $1"%", $NF}'

echo
( cd "$OUT" && gcov -n ./*.gcno 2>/dev/null ) | grep -A1 "src/droid-device.c" | tail -1 | sed 's/^/  total: /'
echo "  droid-pcm.c: no test binary - it opens the HAL to do anything at all"
