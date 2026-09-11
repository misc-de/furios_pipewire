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
# What the numbers do and do not mean: the card and the node are measured
# against a configuration file and a HAL stand-in (tests/hal-stub.c), never
# against the phone. Every line is reached, and that says the decisions around
# the hardware behave - the ring buffer, the give-up rule, the latency
# arithmetic, the routing. It says nothing about the HAL itself.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/src/pulseaudio-modules-droid-modern/src/common"
OUT=${OUT:-$(mktemp -d)}

command -v gcov >/dev/null || { echo "gcov not installed" >&2; exit 1; }
[ -f "$ROOT/poc/spa-droid/build/libdroid-common.a" ] || {
    echo "build the plugin first: ninja -C poc/spa-droid/build" >&2; exit 1; }
[ -f "$ROOT/poc/spa-droid/build/libdroid-config-only.a" ] || {
    ninja -C "$ROOT/poc/spa-droid/build" test-droid-pcm >/dev/null; }

cc -I"$ROOT/poc/spa-droid" -I"$ROOT/poc/spa-droid/compat" \
   -I"$SRC" -I"$SRC/include" -I/usr/include/android -I/usr/include/spa-0.2 \
   -std=gnu11 -O0 -g --coverage \
   -DANDROID_VERSION_MAJOR=11 -DANDROID_VERSION_MINOR=0 -DANDROID_VERSION_PATCH=0 \
   -DHAVE_CONFIG_H -Wno-attributes -Wno-int-conversion -Wno-unused-parameter \
   -DTEST_FIXTURE="\"$ROOT/poc/spa-droid/tests/audio-policy-fixture.xml\"" \
   -DTEST_FIXTURE_NO_PRIMARY="\"$ROOT/poc/spa-droid/tests/audio-policy-no-primary.xml\"" \
   -DTEST_FIXTURE_TOO_MANY="\"$ROOT/poc/spa-droid/tests/audio-policy-too-many.xml\"" \
   "$ROOT/poc/spa-droid/tests/test-droid-device.c" \
   -L"$ROOT/poc/spa-droid/build" -l:libdroid-common.a \
   -lexpat -lhybris-common -lhardware -lpthread -lm \
   -o "$OUT/test-cov"

( cd "$OUT" && ./test-cov >/dev/null )

# The compat layer is compiled straight into its test rather than linked from
# the static library, because gcov needs the instrumented objects.
cc -I"$ROOT/poc/spa-droid" -I"$ROOT/poc/spa-droid/compat" \
   -I"$SRC" -I"$SRC/include" -I/usr/include/android -I/usr/include/spa-0.2 \
   -std=gnu11 -O0 -g --coverage \
   -DANDROID_VERSION_MAJOR=11 -DANDROID_VERSION_MINOR=0 -DANDROID_VERSION_PATCH=0 \
   -DHAVE_CONFIG_H -Wno-attributes -Wno-int-conversion -Wno-unused-parameter \
   "$ROOT/poc/spa-droid/tests/test-compat.c" \
   "$ROOT/poc/spa-droid/compat/pa-audio.c" \
   "$ROOT/poc/spa-droid/compat/pa-compat.c" \
   "$ROOT/poc/spa-droid/compat/pa-containers.c" \
   -lpthread -lm -o "$OUT/test-compat-cov"

( cd "$OUT" && ./test-compat-cov >/dev/null )

# The node's test replaces the HAL half with tests/hal-stub.c, so it links the
# configuration half only - see the note in poc/spa-droid/meson.build.
cc -I"$ROOT/poc/spa-droid" -I"$ROOT/poc/spa-droid/compat" \
   -I"$SRC" -I"$SRC/include" -I/usr/include/android -I/usr/include/spa-0.2 \
   -std=gnu11 -O0 -g --coverage \
   -DANDROID_VERSION_MAJOR=11 -DANDROID_VERSION_MINOR=0 -DANDROID_VERSION_PATCH=0 \
   -DHAVE_CONFIG_H -Wno-attributes -Wno-int-conversion -Wno-unused-parameter \
   -DTEST_FIXTURE="\"$ROOT/poc/spa-droid/tests/audio-policy-fixture.xml\"" \
   -DTEST_FIXTURE_NO_PRIMARY="\"$ROOT/poc/spa-droid/tests/audio-policy-no-primary.xml\"" \
   "$ROOT/poc/spa-droid/tests/test-droid-pcm.c" \
   "$ROOT/poc/spa-droid/tests/hal-stub.c" \
   -L"$ROOT/poc/spa-droid/build" -l:libdroid-config-only.a \
   -lexpat -lpulse -lhybris-common -lhardware -lpthread -lm \
   -o "$OUT/test-pcm-cov"

( cd "$OUT" && ./test-pcm-cov >/dev/null )

echo
echo "== droid-device.c, line coverage per function =="
( cd "$OUT" && gcov -f -n ./test-cov-*.gcno 2>/dev/null ) \
  | awk '/^Function/{fn=$2} /^Lines executed/{print $0, fn}' \
  | grep -E "'(route_priority|route_description|default_route|apply_route_props|build_route_body|build_profile|collect_routes|set_route|set_profile|impl_[a-z_]+|emit_[a-z_]+|params_changed|mix_port_of)'" \
  | sed "s/Lines executed://; s/of //" \
  | sort -t: -k1 -rn \
  | awk -F"[ %]" '{printf "  %7s  %s\n", $1"%", $NF}'

echo
( cd "$OUT" && gcov -n ./test-cov-*.gcno 2>/dev/null ) | grep -A1 "src/droid-device.c" | tail -1 | sed 's/^/  droid-device.c: /'

echo
echo "== droid-pcm.c, line coverage per function =="
( cd "$OUT" && gcov -f -n ./test-pcm-cov-*.gcno 2>/dev/null ) \
  | awk '/^Function/{fn=$2} /^Lines executed/{print $0, fn}' \
  | grep -E "'(hal_open|hal_open_input|hal_close|latency_[a-z_]+|set_timeout|timer_[a-z_]+|on_timeout|writer_[a-z_]+|reader_thread|process_capture|port_[a-z_]+|apply_[a-z_]+|reapply_audio_source|registry_[a-z_]+|droid_node_set_route|emit_[a-z_]+|impl_[a-z_]+)'" \
  | sed "s/Lines executed://; s/of //" \
  | sort -t: -k1 -rn \
  | awk -F"[ %]" '{printf "  %7s  %s\n", $1"%", $NF}'

echo
( cd "$OUT" && gcov -n ./test-pcm-cov-*.gcno 2>/dev/null ) | grep -A1 "src/droid-pcm.c" | tail -1 | sed 's/^/  droid-pcm.c: /'

echo
echo "== the compat layer =="
for f in pa-audio.c pa-compat.c pa-containers.c; do
    ( cd "$OUT" && gcov -n ./test-compat-cov-*.gcno 2>/dev/null ) \
      | grep -A1 "compat/$f" | tail -1 | sed "s/^/  $f: /"
done

echo
echo "== the WirePlumber scripts =="
if command -v lua5.4 >/dev/null 2>&1; then
    for t in "$ROOT"/tests/lua/test-*.lua; do
        [ -e "$t" ] || continue
        TEST_ROOT="$ROOT" LUA_COVERAGE=1 lua5.4 "$t" 2>/dev/null | grep "% of"
    done
else
    echo "  skipped - lua5.4 not installed"
fi

echo
echo "== the Python parts =="
"$ROOT/tests/coverage-python.sh" 2>/dev/null || echo "  could not be measured"

echo
echo "== the shell parts =="
"$ROOT/tests/coverage-shell.sh" 2>/dev/null || echo "  audioctl could not be measured"
"$ROOT/tests/coverage-shell.sh" "$ROOT/tools/furios-audio-callaudio-refresh" \
    "$ROOT/tests/test-callaudio-refresh.sh" 2>/dev/null \
    || echo "  furios-audio-callaudio-refresh could not be measured"
"$ROOT/tests/coverage-shell.sh" "$ROOT/tools/furios-audio-helper" \
    "$ROOT/tests/test-helper.sh" 2>/dev/null \
    || echo "  furios-audio-helper could not be measured"
