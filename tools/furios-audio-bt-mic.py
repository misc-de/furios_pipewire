#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
"""Record through the headset that is already on your ears.

Start a voice memo while wearing Bluetooth earbuds and the phone records its
own microphone - the one lying on the table, pointed at the table. Nothing is
broken when that happens: A2DP is a playback profile and has no microphone at
all, so the headset genuinely has nothing to offer until the card is put into
a hands-free profile, and on this device not even then. SCO does not cross HCI
here; what arrives through BlueZ is a loopback node full of digital silence.
The microphone that works goes through the HAL, and reaching it takes four
steps that nothing performs on its own outside a call:

    the hands-free profile, the negotiated codec announced to the nodes, the
    phone card routed to input-bluetooth_sco_headset, and a stream held on
    bluez_output so that an air link exists underneath all of it

"audioctl bt-mic on" is those four steps and has been measured and heard since
2026-09-12. This service is only the question of WHEN - it watches for a
recording and runs them, then puts everything back. Keeping the mechanism in
audioctl is deliberate: one implementation of the four steps, one place where
the next fix lands, and a handle that still works when this service is off.

WirePlumber has a setting for this idea - "bluetooth.autoswitch-to-headset-
profile", whose own description is "switch to headset mode when recording".
It is off in 51-bluez-ofono.conf and must stay off: it switches the card and
hands the recorder bluez_input.<address>, the silent host side. Picking that
looks like it worked, which is worse than no microphone at all. The switch has
to move the PHONE card's route instead, and that is what this does.

What it costs, measured on 2026-09-13 by switching in the middle of a running
recording (48 kHz, room noise, energy above 8.2 kHz as the marker - the SCO
path is band-limited at 7.5 kHz and cannot carry any):

    t = 0.5 .. 5.0 s   phone microphone     25 .. 47 % above 8.2 kHz
    t = 5.5 .. 7.0 s   nothing at all       RMS 0.0, about 1.5 s of silence
    t = 7.0 s onwards  headset microphone   0.002 % above 8.2 kHz

So the switch does take effect on a stream that is already open - the HAL
reopens the input underneath the recorder - but the first seconds of the file
are the phone, and there is a gap where the path is being rebuilt. That is
inherent to reacting: the recording is what tells us a microphone is wanted,
and by then it has started. Say the first sentence twice, or wait two seconds.

What it does NOT cost, also measured that day: music does not fall onto the
loudspeaker. The profile change destroys the Bluetooth sink and builds a new
one, and every stream on it follows the new node (sink 703 -> 725, both a
test stream and a paused player). It plays on through the headset, mono and
16 kHz for as long as the recording runs, and returns to stereo afterwards.
There is nothing to pause here, unlike a disconnect.

A call is none of this service's business. droid-bluetooth-call.lua sets the
same ports during a call, furios-audio-sco-hold holds the same link, and
callaudiod leaves a call without audio in either direction if a second party
touches the card underneath it. So while the phone card is in its voicecall
profile, this stays out of the way entirely.
"""

import os
import shutil
import signal
import subprocess

import gi

gi.require_version("Gio", "2.0")
from gi.repository import GLib  # noqa: E402

# GLib.unix_signal_add has been moved to its own module and warns twice on
# every start where the old spelling still works. Both spellings exist in the
# wild, so ask for the new one and keep the old as the fallback.
try:
    gi.require_version("GLibUnix", "2.0")
    from gi.repository import GLibUnix  # noqa: E402

    unix_signal_add = GLibUnix.signal_add
except (ValueError, ImportError):
    unix_signal_add = GLib.unix_signal_add

# The setting that turns this on, declared in 51-bluez-ofono.conf so that
# WirePlumber knows the name and wpctl can show it. Off by default, like its
# sibling for calls: a switch that takes the headset out of stereo deserves to
# be asked for once.
SETTING = "furios.bluetooth-mic-routing"

# The phone's own capture node. Everything recorded on this device arrives
# here - emilia's voice memo was measured on it on 2026-09-13 (Source: 55),
# and it is the default source, so an application that simply records gets it
# without choosing anything. That is what makes a route change enough: the
# recorder never learns that Bluetooth was involved.
SOURCE = "droid-source"

# The profile the phone card wears during a call. While it is up, the call
# path owns the card and this service does nothing at all.
CALL_PROFILE = "voicecall"

# How long to keep the hands-free profile after the last recording ends.
# Letting go immediately is right for music - stereo comes back a moment after
# the memo - but two memos in a row are common, and rebuilding the link costs
# the 1.5 s gap measured above. A short grace covers the second memo without
# leaving music in mono long enough to notice.
GRACE_MS = 2500

# Events can be missed - a server restart, this service starting into a
# recording that is already running - so the state is compared with reality on
# a slow timer as well. Nothing about a recording needs to be noticed within
# seconds; what matters is that nothing is left switched forever.
RECHECK_MS = 30000

# The ceiling audioctl puts on its own hold, in seconds. A recording still
# running after three hours is not a voice memo, and a hold nobody takes off
# is how a headset ends up narrow-band for an afternoon.
MAX_HOLD_S = 3 * 60 * 60

# How long to wait before checking that the headset really went back to A2DP,
# and how many times to ask again before saying it out loud.
VERIFY_MS = 1500
VERIFY_TRIES = 3

# A hands-free card that nobody is using is only acted on after this many slow
# checks in a row - a minute of it, not a moment. A call routes the headset
# before callaudiod has set the phone card's own profile, and undoing that
# inside the gap would take the call off the headset it was just put on.
IDLE_CHECKS = 2


def log(msg):
    print(msg, flush=True)


def pactl(*args, timeout=5):
    """Ask pactl something. None means the question could not be asked."""
    try:
        done = subprocess.run(("pactl",) + args, capture_output=True,
                              text=True, timeout=timeout)
    except (OSError, subprocess.SubprocessError) as err:
        log("could not ask pactl %s: %s" % (" ".join(args), err))
        return None
    return done.stdout


def source_index():
    """The index droid-source currently has, as a string, or None.

    Indices are not stable: every profile change builds new nodes, and the
    number that was droid-source ten minutes ago can belong to a Bluetooth
    sink now. So it is read again every time rather than remembered.
    """
    out = pactl("list", "short", "sources")
    if out is None:
        return None
    for line in out.splitlines():
        fields = line.split("\t")
        if len(fields) > 1 and fields[1] == SOURCE:
            return fields[0]
    return None


def recorders():
    """What is recording from the phone's microphone right now.

    Returns a list of names for the log - empty means nobody. Streams on any
    other source are ignored: a monitor being read is not somebody wanting a
    microphone, and droid-voip-source belongs to the call path.
    """
    index = source_index()
    out = pactl("list", "source-outputs")
    if index is None or out is None:
        return []

    found, on_source, name = [], False, None
    for line in out.splitlines():
        stripped = line.strip()
        if stripped.startswith("Source Output #"):
            if on_source:
                found.append(name or "something")
            on_source, name = False, None
        elif stripped.startswith("Source: "):
            on_source = stripped[len("Source: "):].strip() == index
        elif stripped.startswith("application.name = "):
            name = stripped[len("application.name = "):].strip().strip('"')
    if on_source:
        found.append(name or "something")
    return found


def card_profile(prefix):
    """The active profile of the first card whose name starts with prefix."""
    out = pactl("list", "cards")
    if out is None:
        return None
    here = False
    for line in out.splitlines():
        stripped = line.strip()
        if stripped.startswith("Name: "):
            here = stripped[len("Name: "):].startswith(prefix)
        elif here and stripped.startswith("Active Profile: "):
            return stripped[len("Active Profile: "):]
    return None


def call_is_up():
    return card_profile("droid") == CALL_PROFILE


def headset_connected():
    return card_profile("bluez_card.") is not None


def hands_free():
    profile = card_profile("bluez_card.")
    return profile is not None and profile.startswith("headset-head-unit")


def hand_held():
    """Is somebody holding the link by hand - "audioctl bt-mic on" from a
    terminal - right now?

    audioctl writes the pid of its hold into XDG_RUNTIME_DIR. That file is
    what tells a headset somebody put into hands-free on purpose apart from
    one left there by something that is no longer running, and the difference
    decides whether it is ours to undo. The service that holds a link during a
    CALL writes no such file, which is why the call profile is asked about
    separately.
    """
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if not runtime:
        return False
    try:
        with open(os.path.join(runtime, "furios-audio-sco-hold.pid")) as fh:
            pid = int(fh.read().strip())
    except (OSError, ValueError):
        return False
    return os.path.isdir("/proc/%d" % pid)


def setting_on():
    """Is the switch on? Anything unreadable counts as off.

    A setting that cannot be read is not a reason to take somebody's music out
    of stereo, and the failure to say so is loud: the headset simply keeps
    recording nothing, which is exactly what it did before this existed.
    """
    try:
        done = subprocess.run(["wpctl", "settings", SETTING],
                              capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.SubprocessError) as err:
        log("could not read %s: %s - staying out of it" % (SETTING, err))
        return False
    for line in done.stdout.splitlines():
        if line.strip().startswith("Value:"):
            return line.split(":", 1)[1].strip().split()[0] == "true"
    return False


class Switch:
    """The headset's hands-free state, and whether it is ours to undo.

    Ownership is the whole point of this class. Somebody who ran
    "audioctl bt-mic on" by hand, or a call that routed the headset itself,
    must not have it taken away again when a recording happens to end - so
    what was already hands-free when the recording started stays untouched.
    """

    def __init__(self, audioctl):
        self.audioctl = audioctl
        self.ours = False
        self.letting_go = None
        # One line per episode, not one per event. pactl reports several
        # changes for a single recording, and "leaving it alone" written eight
        # times buries the line that matters.
        self.said_foreign = False

    def _run(self, *args):
        try:
            done = subprocess.run([self.audioctl, "bt-mic"] + list(args),
                                  capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as err:
            log("audioctl bt-mic %s failed: %s" % (" ".join(args), err))
            return False
        for line in done.stdout.splitlines():
            if line.startswith("bt-mic: "):
                log("  " + line[len("bt-mic: "):])
        return done.returncode == 0

    def cancel_letting_go(self):
        """A recording started again inside the grace period."""
        if self.letting_go is not None:
            GLib.source_remove(self.letting_go)
            self.letting_go = None
            return True
        return False

    def take(self, who):
        if self.cancel_letting_go():
            log("recording again (%s) - keeping the headset" % who)
            return
        if self.ours:
            return
        if hands_free():
            # Already in hands-free, and not by us. Nothing to do and nothing
            # to undo later - the recording gets the headset either way.
            if not self.said_foreign:
                log("%s is recording and the headset is already hands-free "
                    "- leaving it alone" % who)
                self.said_foreign = True
            return
        log("%s is recording - taking the headset into hands-free" % who)
        if self._run("on", str(MAX_HOLD_S)):
            self.ours = True
        else:
            log("could not switch - the recording keeps the phone microphone")

    def release(self, grace=True):
        self.said_foreign = False
        if not self.ours:
            return
        if not grace:
            self._let_go()
            return
        if self.letting_go is None:
            self.letting_go = GLib.timeout_add(GRACE_MS, self._let_go)

    def _let_go(self):
        self.letting_go = None
        if not self.ours:
            return False
        log("recording over - giving the headset back")
        self._give_back()
        return False

    def _give_back(self):
        self._run("off")
        self.ours = False
        # Asking is not the same as arriving. A card that has lost its A2DP
        # profiles answers "No such entity" and stays hands-free, and then
        # every track after the memo plays mono at 16 kHz with nothing saying
        # why - measured on 2026-09-13, after a WirePlumber restart had left
        # the card with only "off" and the two headset profiles.
        GLib.timeout_add(VERIFY_MS, self._verify, 1)

    def _verify(self, attempt):
        if self.ours:
            # A new recording started while this was pending; it is switched
            # on again on purpose and must not be undone here.
            return False
        if not hands_free():
            return False
        if attempt <= VERIFY_TRIES:
            # The profile can come back a moment later - the A2DP transport
            # needs one - so ask again before making noise about it.
            self._run("off")
            GLib.timeout_add(VERIFY_MS, self._verify, attempt + 1)
            return False
        log("WARNING the headset is still in hands-free - music will be mono "
            "at 16 kHz until its A2DP profiles come back.")
        log("         Usually cured by: bluetoothctl disconnect <address>; "
            "bluetoothctl connect <address>")
        return False

    def give_back_leftover(self):
        """Hands-free with nobody recording, nobody calling, nobody holding.

        Something left the headset this way and is no longer around to undo
        it - a WirePlumber restart picking the only profile that was available
        at that instant is the case measured on 2026-09-13. Nothing will ever
        put it back on its own, and what the owner notices is that their music
        has gone mono for no reason they can see.
        """
        log("the headset sits in hands-free with nothing recording "
            "- putting it back to A2DP")
        self._give_back()


class Watcher:
    """The decision table, kept out of main() so that it can be asked.

    Every line of it was paid for on the phone: the call that must not be
    touched, the headset somebody put into hands-free by hand, the one left
    there by something that is gone, and the recording that is the only reason
    to switch anything at all.
    """

    def __init__(self, switch):
        self.switch = switch
        self.idle = 0

    def look(self, slow=False):
        """What is true right now, and what that should mean.

        slow=True is the timer rather than an event, and it is the only caller
        allowed to conclude that a hands-free headset has been forgotten: that
        takes seeing the same thing twice, not once.
        """
        who = recorders()
        if not who:
            self.switch.release()
            if (slow and not self.switch.ours and not call_is_up()
                    and hands_free() and not hand_held()):
                self.idle += 1
                if self.idle >= IDLE_CHECKS:
                    self.idle = 0
                    self.switch.give_back_leftover()
            else:
                self.idle = 0
            return
        self.idle = 0
        if call_is_up():
            # The call path owns the card: droid-bluetooth-call.lua sets these
            # same ports and furios-audio-sco-hold holds this same link. Two
            # services setting one card is how a call ends up with no audio in
            # either direction.
            return
        if not setting_on():
            return
        if not headset_connected():
            return
        self.switch.take(", ".join(sorted(set(who))))


def main():
    for tool in ("pactl", "wpctl"):
        if shutil.which(tool) is None:
            log("%s is not installed - nothing to watch with" % tool)
            return
    audioctl = shutil.which("audioctl")
    if audioctl is None:
        log("audioctl is not installed - nothing to switch with")
        return

    watcher = Watcher(Switch(audioctl))
    switch = watcher.switch

    try:
        events = subprocess.Popen(["pactl", "subscribe"],
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True)
    except OSError as err:
        log("could not subscribe to pactl events: %s" % err)
        return

    loop = GLib.MainLoop()
    rest = [""]

    def on_event(channel, condition):
        # Non-blocking on purpose: a partial line would otherwise stop the
        # whole loop inside readline, and with it the letting-go timer.
        if condition & (GLib.IOCondition.HUP | GLib.IOCondition.ERR):
            # The server went away, or pactl did. Give the headset back before
            # leaving - systemd restarts this, and a headset left hands-free
            # by a service that is no longer running is nobody's to fix.
            log("the event stream ended - giving up for now")
            switch.release(grace=False)
            loop.quit()
            return False
        try:
            chunk = os.read(channel.unix_get_fd(), 4096).decode("utf-8", "replace")
        except OSError:
            return True
        if not chunk:
            return True
        rest[0] += chunk
        lines = rest[0].split("\n")
        rest[0] = lines.pop()
        if any("source-output" in line for line in lines):
            watcher.look()
        return True

    channel = GLib.IOChannel.unix_new(events.stdout.fileno())
    channel.set_encoding(None)
    channel.set_flags(GLib.IOFlags.NONBLOCK)
    GLib.io_add_watch(channel, GLib.PRIORITY_DEFAULT,
                      GLib.IOCondition.IN | GLib.IOCondition.HUP |
                      GLib.IOCondition.ERR, on_event)

    def recheck():
        watcher.look(slow=True)
        return True

    GLib.timeout_add(RECHECK_MS, recheck)

    def bye(*_):
        # Never leave the earbuds hands-free behind a stopped service.
        switch.release(grace=False)
        if events.poll() is None:
            events.terminate()
        loop.quit()
        return GLib.SOURCE_REMOVE

    unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, bye)
    unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGINT, bye)

    log("watching for recordings on %s" % SOURCE)
    # A recording that is already running when this starts - a restart in the
    # middle of a memo - is found here rather than waited for.
    watcher.look()
    loop.run()


if __name__ == "__main__":
    main()
