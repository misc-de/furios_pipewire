#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
"""Give PipeWire's hands-free gateway a call index a car kit will accept.

PipeWire answers a car's AT+CLCC ("which calls are there?") with
"+CLCC: <idx>,<dir>,<state>,...", and <idx> comes from call->index - which
spa/plugins/bluez5/modemmanager.c never sets. Every call therefore goes out as
call 0. 3GPP TS 27.007 numbers calls from 1, and a strict hands-free unit drops
the entry: measured on 2026-09-25 with an Audi A6 (2013, MMI 3G, HFP 1.5),
which polls AT+CLCC once a second. The phone showed an active call, the SCO
link was up, the HAL routed to "BT SCO" - and the car displayed nothing, kept
the radio playing and neither played nor recorded a word. With index 1 the
display, the radio and both directions of speech were right at once. Earbuds
never look at the list, which is why they always worked.

Still true in PipeWire 1.6.6 and on master. The real fix belongs upstream: give
each call a number when ModemManager announces it. Until then, this.

What it does: before WirePlumber starts, it copies the system's
libspa-bluez5.so to $XDG_RUNTIME_DIR/furios-audio/spa-0.2/bluez5/ and changes
one instruction in the copy - the load of call->index for the +CLCC reply,
"ldr w2, [x20, #16]", becomes "mov w2, #1". Nothing under /usr is touched, and
no string is edited: the format strings may share their tails with other
literals (the linker merges string suffixes), so editing one could change text
somewhere else.

WirePlumber's drop-in puts that directory IN FRONT of the system one in
SPA_PLUGIN_DIR. PipeWire walks the list, so everything else - the codecs, the
droid plugin, every other SPA plugin - still comes from the system, and when
there is no copy WirePlumber simply loads the original.

That is also the safety rule: the instruction sequence around the load has to
appear exactly once in the file, byte for byte. After a PipeWire update that
changes the code, it will not, no copy is written, and the phone falls back to
the unpatched plugin - the car goes quiet again, but nothing else breaks, and
this says so in the journal. The copy lives on tmpfs and is rebuilt at every
WirePlumber start, so it can never outlive the library it was made from.

Known limit: every call is number 1. With a second call waiting, a car shows
both as the same call. One call at a time - the case this is for - is right.
"""

import glob
import os
import sys
import tempfile

# The loop in backend-native.c that answers AT+CLCC, as built in Debian's
# PipeWire 1.6.6 (aarch64). It loads the fields of struct call for
# rfcomm_send_reply():
#   ldr  x22, [x20, #48]     number
#   ldp  w3, w4, [x20, #60]  direction, state
#   ldrb w5, [x20, #68]      multiparty
#   ldr  w2, [x20, #16]      index            <- this one
#   cbnz x22, ...            with or without a number
SIGNATURE = bytes.fromhex("961a40f9 83924729 85124139 821240b9 16fdffb5"
                          .replace(" ", ""))
LOAD_INDEX = bytes.fromhex("821240b9")   # ldr w2, [x20, #16]
INDEX_ONE = bytes.fromhex("22008052")    # mov w2, #1
OFFSET_IN_SIGNATURE = 12

SYSTEM_GLOB = "/usr/lib/*/spa-0.2/bluez5/libspa-bluez5.so"


def log(message):
    print(f"furios-audio-bluez5-fix: {message}", file=sys.stderr)


def patch(data):
    """The patched library, or None with the reason when it does not fit."""
    first = data.find(SIGNATURE)
    if first < 0:
        return None, "the +CLCC code is not the one this was made for"
    if data.find(SIGNATURE, first + 1) >= 0:
        return None, "the +CLCC code appears more than once"
    at = first + OFFSET_IN_SIGNATURE
    assert data[at:at + 4] == LOAD_INDEX
    return data[:at] + INDEX_ONE + data[at + 4:], None


def overlay_path(runtime_dir):
    return os.path.join(runtime_dir, "furios-audio", "spa-0.2", "bluez5",
                        "libspa-bluez5.so")


def run(system_glob=SYSTEM_GLOB, runtime_dir=None):
    runtime_dir = runtime_dir or os.environ.get("XDG_RUNTIME_DIR")
    if not runtime_dir:
        log("no XDG_RUNTIME_DIR - leaving the plugin as it is")
        return 0
    target = overlay_path(runtime_dir)

    # Whatever happens next, an old copy must not survive: it was made from a
    # library that may since have been replaced.
    try:
        os.unlink(target)
    except FileNotFoundError:
        pass

    found = glob.glob(system_glob)
    if len(found) != 1:
        log(f"expected one libspa-bluez5.so, found {len(found)} - "
            "WirePlumber loads the original")
        return 0
    with open(found[0], "rb") as f:
        data = f.read()

    patched, why = patch(data)
    if patched is None:
        log(f"{why} - WirePlumber loads the original, "
            "and a car kit will not see calls")
        return 0

    os.makedirs(os.path.dirname(target), mode=0o700, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(target))
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(patched)
        os.chmod(tmp, 0o644)
        os.replace(tmp, target)
    except BaseException:
        os.unlink(tmp)
        raise
    log(f"+CLCC call index fixed in {target}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(run())
    except Exception as e:  # never keep WirePlumber from starting
        log(f"failed ({e}) - WirePlumber loads the original")
        sys.exit(0)
