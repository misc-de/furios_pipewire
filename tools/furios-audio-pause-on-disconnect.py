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
"""

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

MPRIS_PREFIX = "org.mpris.MediaPlayer2."


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
        except GLib.Error:
            continue
        if status == "Playing":
            out.append(name)
    return out


def pause_all(session):
    players = playing_players(session)
    if not players:
        print("bluetooth gone, nothing was playing", flush=True)
        return
    for name in players:
        try:
            session.call_sync(
                name, "/org/mpris/MediaPlayer2",
                "org.mpris.MediaPlayer2.Player", "Pause", None, None,
                Gio.DBusCallFlags.NONE, 2000, None,
            )
            print("bluetooth gone, paused %s" % name[len(MPRIS_PREFIX):],
                  flush=True)
        except GLib.Error as err:
            print("could not pause %s: %s" % (name, err), flush=True)


def main():
    system = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    session = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    def on_props(_conn, _sender, path, _iface, _signal, params):
        iface, changed, _invalidated = params.unpack()
        if iface != "org.bluez.Device1":
            return
        if changed.get("Connected") is False:
            print("device %s disconnected" % path.rsplit("/", 1)[-1], flush=True)
            pause_all(session)

    system.signal_subscribe(
        "org.bluez", "org.freedesktop.DBus.Properties", "PropertiesChanged",
        None, None, Gio.DBusSignalFlags.NONE, on_props,
    )
    print("watching BlueZ for disconnects", flush=True)
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
