#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
"""Hold an SCO link open for the length of a phone call.

A hands-free PROFILE is not a hands-free LINK, and that difference cost a day.

droid-bluetooth-call.lua does everything that looks like the job: it puts the
headset into headset-head-unit, tells the nodes which codec was negotiated, and
routes the phone card to output-bluetooth_sco / input-bluetooth_sco_headset.
Measured in a real call on 2026-09-12 it did all of that two seconds in and
held it steady to the end - and the call was silent in both directions.

Nothing was wrong with the routing. What was missing was a link for it to be
routed onto. On this chip SCO does not cross HCI: BlueZ only puts the air link
up, and it puts it up while a stream is active on bluez_output.* - no stream,
no link. In a call nobody opens one, because the voice path runs modem <-> DSP
and never reaches the host, so there is no stream to be had and the profile
sits there with nothing underneath it. The same call, repeated with a stream of
zeroes held on bluez_output, was heard in both directions.

So that is all this does: while a call is up and the headset is in a hands-free
profile, it keeps a silent stream on the Bluetooth output. The HAL carries the
sound; this only keeps the road open.

It deliberately does NOT set the profile or the routes. That is the Lua
script's job, it is hard to get right around callaudiod, and two things setting
the same ports is the failure mode that produced a call with no audio at all
once before. If the profile never arrives, this gives up and says so: a call on
the earpiece is a nuisance, a call nobody can hear is not.

Why not inside WirePlumber: the monitor cannot start a stream of its own
without a good deal of machinery, and an error in there takes the whole script
down with it - no card, no nodes, no sound at all. A separate process can fail
on its own.
"""

import os
import shutil
import signal
import subprocess

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

# GLib.unix_signal_add has been moved to its own module and warns twice on
# every start where the old spelling still works. Both spellings exist in the
# wild, so ask for the new one and keep the old as the fallback.
try:
    gi.require_version("GLibUnix", "2.0")
    from gi.repository import GLibUnix  # noqa: E402

    unix_signal_add = GLibUnix.signal_add
except (ValueError, ImportError):
    unix_signal_add = GLib.unix_signal_add

# The profiles that mean "this card can carry a call". Anything else - and
# a2dp-sink in particular - must NOT be held: in the A2DP profile bluez_output
# exists too, and a stream of zeroes on it would hold music Bluetooth open for
# the length of a call instead of building an SCO link. Same sink name, wrong
# road entirely.
HANDS_FREE_PREFIX = "headset-head-unit"

# How long to wait for the hands-free profile to appear after a call starts,
# and how often to look. The Lua script waits for the card to go quiet before
# it switches - measured at about +1.05 s - and callaudiod's own sequence can
# push that out. Twelve seconds is far longer than that and still over well
# before anyone has finished saying hello twice.
POLL_MS = 250
MAX_WAIT_MS = 12000

# If the stream dies while the call is still up - a profile change tears the
# node out from under it - put it back, but not forever. A loop that restarts a
# failing paplay several times a second for the length of a call is worse than
# a call on the earpiece.
MAX_RESTARTS = 5

# A call that never ends as far as this service is concerned - a missed
# CallRemoved, ofono restarting underneath it - must not leave the earbuds in a
# hands-free profile for the rest of the day, where music is narrow-band and
# mono and somebody finds it hours later and blames the headset.
MAX_HOLD_S = 3 * 60 * 60


def log(msg):
    print(msg, flush=True)


def hands_free_sink():
    """The Bluetooth sink to hold, but only if the card is in a hands-free
    profile right now. None means "not yet" or "not this profile"."""
    try:
        cards = subprocess.run(["pactl", "list", "cards"],
                               capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError) as err:
        log("could not ask pactl about cards: %s" % err)
        return None

    in_bluez, profile = False, None
    for line in cards.splitlines():
        stripped = line.strip()
        if stripped.startswith("Name: "):
            in_bluez = stripped[len("Name: "):].startswith("bluez_card.")
        elif in_bluez and stripped.startswith("Active Profile: "):
            profile = stripped[len("Active Profile: "):]
            break
    if profile is None or not profile.startswith(HANDS_FREE_PREFIX):
        return None

    try:
        sinks = subprocess.run(["pactl", "list", "short", "sinks"],
                               capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError) as err:
        log("could not ask pactl about sinks: %s" % err)
        return None
    for line in sinks.splitlines():
        fields = line.split("\t")
        if len(fields) > 1 and fields[1].startswith("bluez_output."):
            return fields[1]
    return None


class Hold:
    """The silent stream, and everything that keeps it honest."""

    def __init__(self):
        self.proc = None
        self.restarts = 0
        self.waiting = False
        self.watch = None
        self.deadline = None

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def start_on(self, sink):
        # --raw and /dev/zero: silence, at the rate SCO runs. paplay and not
        # pw-play because that is what was measured holding a link here.
        try:
            self.proc = subprocess.Popen(
                ["paplay", "--raw", "--format=s16le", "--rate=16000",
                 "--channels=1", "--device=" + sink, "/dev/zero"],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, start_new_session=True)
        except OSError as err:
            log("could not start the hold: %s" % err)
            self.proc = None
            return
        log("holding the link on %s (pid %d)" % (sink, self.proc.pid))
        self.watch = GLib.child_watch_add(GLib.PRIORITY_DEFAULT, self.proc.pid,
                                          self._died)
        if self.deadline is None:
            self.deadline = GLib.timeout_add_seconds(MAX_HOLD_S, self._too_long)

    def _died(self, pid, status, *_):
        self.watch = None
        if self.proc is None:          # stopped on purpose
            return
        self.proc = None
        if self.restarts >= MAX_RESTARTS:
            log("the hold keeps dying - giving up for this call")
            return
        self.restarts += 1
        log("the hold died (status %s) - putting it back (%d/%d)"
            % (status, self.restarts, MAX_RESTARTS))
        self.begin()

    def _too_long(self):
        log("a call has been up for %d hours - dropping the hold" % (MAX_HOLD_S // 3600))
        self.stop()
        return False

    def begin(self, waited=0):
        """Wait for the hands-free profile, then hold. Called on every call."""
        if self.running():
            return False
        sink = hands_free_sink()
        if sink:
            self.waiting = False
            self.start_on(sink)
            return False
        if waited >= MAX_WAIT_MS:
            self.waiting = False
            log("no hands-free profile after %d s - not holding anything. "
                "Is furios.bluetooth-call-routing on?" % (MAX_WAIT_MS // 1000))
            return False
        self.waiting = True
        GLib.timeout_add(POLL_MS, self.begin, waited + POLL_MS)
        return False

    def stop(self):
        self.waiting = False
        self.restarts = 0
        if self.deadline is not None:
            GLib.source_remove(self.deadline)
            self.deadline = None
        proc, self.proc = self.proc, None
        if self.watch is not None:
            GLib.source_remove(self.watch)
            self.watch = None
        if proc is None or proc.poll() is not None:
            return
        try:
            # The stream was started in its own session so a profile change
            # cannot take it down with the shell; kill the group it made.
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        except OSError:
            proc.terminate()
        log("hold stopped")


def existing_calls(system):
    """Calls that are already up when this starts - a restart mid-call."""
    found = set()
    try:
        modems = system.call_sync(
            "org.ofono", "/", "org.ofono.Manager", "GetModems", None,
            GLib.VariantType("(a(oa{sv}))"), Gio.DBusCallFlags.NONE, 5000, None,
        ).unpack()[0]
    except GLib.Error as err:
        log("could not ask ofono for modems: %s" % err)
        return found
    for path, _props in modems:
        try:
            calls = system.call_sync(
                "org.ofono", path, "org.ofono.VoiceCallManager", "GetCalls",
                None, GLib.VariantType("(a(oa{sv}))"),
                Gio.DBusCallFlags.NONE, 5000, None,
            ).unpack()[0]
        except GLib.Error:
            continue
        found.update(call_path for call_path, _ in calls)
    return found


def main():
    for tool in ("pactl", "paplay"):
        if shutil.which(tool) is None:
            log("%s is not installed - nothing to hold a link with" % tool)
            return

    system = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    hold = Hold()
    calls = existing_calls(system)

    def on_added(_conn, _sender, _path, _iface, _signal, params):
        path = params.unpack()[0]
        first = not calls
        calls.add(path)
        if first:
            log("call %s - waiting for the hands-free profile"
                % path.rsplit("/", 1)[-1])
            hold.begin()

    def on_removed(_conn, _sender, _path, _iface, _signal, params):
        calls.discard(params.unpack()[0])
        if not calls:
            log("call ended")
            hold.stop()

    system.signal_subscribe("org.ofono", "org.ofono.VoiceCallManager",
                            "CallAdded", None, None, Gio.DBusSignalFlags.NONE,
                            on_added)
    system.signal_subscribe("org.ofono", "org.ofono.VoiceCallManager",
                            "CallRemoved", None, None,
                            Gio.DBusSignalFlags.NONE, on_removed)

    loop = GLib.MainLoop()

    def bye(*_):
        # Stop the hold on the way out, or the earbuds stay in hands-free after
        # this service is stopped or the session ends.
        hold.stop()
        loop.quit()
        return GLib.SOURCE_REMOVE

    unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, bye)
    unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGINT, bye)

    if calls:
        log("started during a call - waiting for the hands-free profile")
        hold.begin()
    log("watching ofono for calls")
    loop.run()


if __name__ == "__main__":
    main()
