#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
"""Pause playback when a Bluetooth audio device disconnects.

Earbuds run out of battery, or you put one back in its case, and without this
the audio simply moves to the next best output - which on a phone is the
loudspeaker, in whatever room you happen to be standing in.

So this pauses instead of rerouting. It watches BlueZ for a device losing its
connection and asks every MPRIS player that is currently playing to pause. That
is the same thing Android does, and it leaves nothing behind: no muted sink, no
changed default, nothing to undo later. The next press of play works normally.

A player without MPRIS cannot be paused this way. That is a real limit and not
worth papering over with a mute, which would be a trap of its own - a phone
that is silent for reasons nobody remembers is worse than one that was briefly
too loud.

One look at the moment of the signal is not enough, though, and that is not a
theory: on 2026-09-12 earbuds ran out of battery at 06:31:30 with a podcast
playing, this service reported "nothing was playing", and the podcast moved to
the loudspeaker and played on for 66 minutes until its owner woke up and
pressed the power key. Reproduced afterwards with the service stopped: the
player stays "Playing" throughout and simply follows the stream onto the
speaker. So the disconnect is not a moment to inspect but a few seconds to
watch - the player may report "Paused" for an instant while its sink is gone,
or be too slow to answer at all on a phone that has just woken up.
"""

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

MPRIS_PREFIX = "org.mpris.MediaPlayer2."

# When to look again after a device is gone, in milliseconds from the signal.
# The first look happens immediately; these are the ones that catch a player
# which was between states, or too slow to answer, when the first one asked.
# It ends well before a podcast could become a nuisance, and long before
# anyone would reconnect their earbuds and press play again on purpose.
RETRY_DELAYS_MS = (400, 900, 1800, 3000, 5000, 8000, 12000)


def playing_players(session):
    """Names of MPRIS players that are playing right now."""
    try:
        names = session.call_sync(
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "ListNames", None,
            GLib.VariantType("(as)"), Gio.DBusCallFlags.NONE, 2000, None,
        ).unpack()[0]
    except GLib.Error as err:
        print("could not list bus names: %s" % err, flush=True)
        return []

    out = []
    for name in names:
        if not name.startswith(MPRIS_PREFIX):
            continue
        try:
            status = session.call_sync(
                name, "/org/mpris/MediaPlayer2",
                "org.freedesktop.DBus.Properties", "Get",
                GLib.Variant("(ss)", ("org.mpris.MediaPlayer2.Player",
                                      "PlaybackStatus")),
                GLib.VariantType("(v)"), Gio.DBusCallFlags.NONE, 2000, None,
            ).unpack()[0]
        except GLib.Error as err:
            # NOT silent: a player that fails to answer within the timeout
            # looks exactly like one that is not playing, and that is how a
            # podcast once ran all morning on the loudspeaker. Say so, and
            # leave it to the retries to ask again.
            print("could not ask %s: %s" % (name[len(MPRIS_PREFIX):], err),
                  flush=True)
            continue
        if status == "Playing":
            out.append(name)
    return out


def pause_all(session, again=False):
    """Pause every player that is playing right now.

    `again` only changes what is printed: the retries would otherwise write
    "nothing was playing" eight times per disconnect, which would bury the one
    line that matters.
    """
    players = playing_players(session)
    if not players:
        if not again:
            print("bluetooth gone, nothing was playing yet - still watching",
                  flush=True)
        return
    for name in players:
        try:
            session.call_sync(
                name, "/org/mpris/MediaPlayer2",
                "org.mpris.MediaPlayer2.Player", "Pause", None, None,
                Gio.DBusCallFlags.NONE, 2000, None,
            )
            print("bluetooth gone, paused %s%s" % (name[len(MPRIS_PREFIX):],
                  " (on a second look)" if again else ""), flush=True)
        except GLib.Error as err:
            print("could not pause %s: %s" % (name, err), flush=True)


class Retry:
    """Keeps looking for a moment after a Bluetooth device disconnects.

    Every look pauses whatever plays right now. Pausing an already paused
    player is harmless, so repeating costs nothing - while missing the one
    instant the player was readable costs a morning.

    It gives up as soon as something connects again: that is somebody putting
    their earbuds back in, and pausing their music a few seconds later would
    be its own kind of haunted phone.
    """

    def __init__(self, session):
        self.session = session
        self.left = len(RETRY_DELAYS_MS)
        self.cancelled = False

    def cancel(self):
        self.cancelled = True

    def tick(self):
        """One more look. True while another one is still due."""
        if self.cancelled:
            return False
        pause_all(self.session, again=True)
        self.left -= 1
        return self.left > 0


def main():
    system = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    session = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    pending = []   # at most one Retry, in a list so the closure can replace it

    def schedule(retry):
        for delay in RETRY_DELAYS_MS:
            GLib.timeout_add(delay, lambda r=retry: (r.tick(), False)[1])

    def on_props(_conn, _sender, path, _iface, _signal, params):
        iface, changed, _invalidated = params.unpack()
        if iface != "org.bluez.Device1":
            return
        connected = changed.get("Connected")
        if connected is False:
            print("device %s disconnected" % path.rsplit("/", 1)[-1], flush=True)
            for old_retry in pending:
                old_retry.cancel()
            pending.clear()
            pause_all(session)
            retry = Retry(session)
            pending.append(retry)
            schedule(retry)
        elif connected is True:
            # Earbuds are back in. Whatever we were still watching for, stop -
            # pausing their music seconds after they pressed play would be
            # worse than the problem this service exists for.
            for old_retry in pending:
                old_retry.cancel()
            pending.clear()

    system.signal_subscribe(
        "org.bluez", "org.freedesktop.DBus.Properties", "PropertiesChanged",
        None, None, Gio.DBusSignalFlags.NONE, on_props,
    )
    print("watching BlueZ for disconnects", flush=True)
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
