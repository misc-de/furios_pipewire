#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# Do the three ways in and out of this repo still agree with each other?
#
# They did not. install.sh knew three units while audioctl knew seven, so a
# script install left the Bluetooth call hold, the Bluetooth microphone, the
# pause-on-disconnect watcher and the callaudiod refresh on the floor - and
# the plugin that makes PipeWire talk to the HAL was not installed at all,
# only described in the README. uninstall.sh had drifted the same way, and
# packaging/build-deb.sh had gone on installing an app that had moved to its
# own repository, so the package could not be built at all.
#
# Nothing here needs root, a phone or a build tree: it reads the scripts.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
# shellcheck source=lib.sh
. "$HERE/lib.sh"

cd "$ROOT"

enthalten() {
    # enthalten <file> <word> -> "yes" / "no"
    if grep -q -- "$2" "$1"; then echo yes; else echo no; fi
}

echo "-- every unit in this repo is installed, removed and packaged"
for unit in furios-*.service; do
    name=${unit%.service}
    check "install.sh installs $name"   yes "$(enthalten install.sh "$unit")"
    check "uninstall.sh removes $name"  yes "$(enthalten uninstall.sh "$unit")"
    check "the package ships $name"     yes "$(enthalten packaging/build-deb.sh "$unit")"
done

echo
echo "-- every program a unit starts is installed, removed and packaged"
# The name in ExecStart is the contract. A unit whose program nobody installs
# fails at the first start with status=203/EXEC, which reads like a broken
# program rather than a missing one.
programme=$(grep -h "^ExecStart=/usr/bin/furios" furios-*.service \
            | sed 's|^ExecStart=/usr/bin/||' | sort -u)
for prog in $programme; do
    check "install.sh installs $prog"  yes "$(enthalten install.sh "$prog")"
    check "uninstall.sh removes $prog" yes "$(enthalten uninstall.sh "$prog")"
    check "the package ships $prog"    yes "$(enthalten packaging/build-deb.sh "$prog")"
done

echo
echo "-- every WirePlumber script is installed, removed and packaged"
for lua in wireplumber/*.lua wireplumber/*.conf; do
    name=$(basename "$lua")
    check "install-hal.sh installs $name" yes "$(enthalten install-hal.sh "$name")"
    check "uninstall.sh removes $name"    yes "$(enthalten uninstall.sh "$name")"
    check "the package ships $name"       yes "$(enthalten packaging/build-deb.sh "$name")"
done

echo
echo "-- the one entry point does the whole job"
check "install.sh builds the plugin" yes "$(enthalten install.sh build-plugin.sh)"
check "install.sh installs the HAL side" yes "$(enthalten install.sh install-hal.sh)"
check "the plugin build pins its upstream commit" yes \
    "$(enthalten tools/build-plugin.sh 'COMMIT=')"
# Nothing the package ships may come from a directory that is not here any
# more: that is how build-deb.sh broke without anybody noticing.
fehlend=0
while read -r quelle; do
    [ -e "$quelle" ] || { fehlend=$((fehlend + 1)); echo "       not in this repo: $quelle"; }
done < <(grep -oE '^install -Dm[0-9]+ [^ "]+' packaging/build-deb.sh | awk '{print $3}')
check "the package installs only files that exist here" 0 "$fehlend"

# The same for the two install scripts. A source path that is one letter off
# fails in the middle of an install, with half the stack in place - and on a
# phone, half a stack is a phone without sound.
fehlend=0
while read -r quelle; do
    case "$quelle" in *'$'*) continue ;; esac
    [ -e "$quelle" ] || { fehlend=$((fehlend + 1)); echo "       not in this repo: $quelle"; }
done < <(grep -hoE 'sudo install -[Dm0-9]+ +[^ "$]+' install.sh install-hal.sh | awk '{print $4}')
check "both install scripts copy only files that exist here" 0 "$fehlend"

summary
