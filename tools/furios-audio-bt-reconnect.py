#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
"""Reconnect a Bluetooth audio device that nobody is reconnecting.

BlueZ dials a paired device on its own in exactly two situations: when the
adapter is powered on, and when a connection was lost to radio trouble. A
device that simply hung up - earbuds going into their case, a headset whose
battery ran out, a car that drove away - is never called again. On Android
something does; on phosh nothing does, so the earbuds sit in your ears, awake
and in range, while the phone plays through its loudspeaker.

Measured on 2026-09-15: the earbuds disconnected at 12:05:21, and at 13:44
they answered a remote name request within a second - both sides ready, no
connection, because neither side had any reason to start one. The adapter was
powered and page-scanning the whole time, so nothing was broken. It was simply
that nobody dialled.

This dials. What it does NOT do is poll: every attempt is a page, which is
seconds of radio and a real cost on a phone that never suspends. So it works
by occasions, of which there are two:

  a disconnect     a short series of attempts with growing gaps, for earbuds
                   that went briefly out of range or into a pocket.

  waking up        one attempt when the phone comes back from idle or from
                   the lock screen - you picked the phone up, so you are
                   there, and so, probably, are the earbuds.

Between the occasions it does nothing at all, which is the point: a phone that
pages a device every thirty seconds all night to find it switched off has
spent the battery this repository has spent months protecting.

The candidate is only ever a device this service watched disconnect. Paired
audio devices on this phone include a car kit and an OBD adapter, and paging
those because somebody's headset is missing would be noise on the air and a
car radio switching on in a parked car. Nothing is dialled that was not there
a moment ago.
"""

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

# Same list as the pause-on-disconnect watcher: the profiles that carry sound.
# A keyboard or a fitness tracker dropping off is not something to dial back.
AUDIO_UUID_PREFIXES = (
    "0000110a",   # AudioSource   (A2DP)
    "0000110b",   # AudioSink     (A2DP)
    "00001108",   # Headset
    "00001112",   # Headset AG
    "0000111e",   # Handsfree
    "0000111f",   # Handsfree AG
)

# Seconds after a disconnect at which to try again. They grow because the
# cost does not: a failed attempt is a page timeout, around ten seconds of
# radio, whether it is the first or the fifth. Four attempts over a quarter of
# an hour cover "walked out of range and came back" and then stop - after that
# the earbuds are off or elsewhere, and waking up is the better occasion.
RETRY_DELAYS_S = (20, 60, 180, 600)

# How long a connection has to last before it counts as wanted. Earbuds with
# multipoint hand themselves to a laptop and drop us again within a couple of
# seconds; dialling them back turns that into a tug of war, with the music
# jumping between two machines. Under this, the device is left alone until the
# next occasion.
KEPT_S = 30

# The shortest gap between two attempts made because the phone woke up.
#
# Not a theory: the first run of this service on the phone connected through
# the waking-up path within seconds of a disconnect, because logind's IdleHint
# had gone false. That occasion is unbounded - a phone is picked up and put
# down all day, and every attempt is a page, ten seconds of radio when the
# device is not there. The series after a disconnect needs no such limit
# because it stops by itself after four.
WAKE_MIN_GAP_S = 300

# How long a device stays worth dialling after it went. Earbuds left in a
# drawer over a weekend must not be paged on every unlock for the rest of the
# month; after a day of nothing, the next connection can be made by hand.
CANDIDATE_TTL_S = 24 * 3600

# BlueZ takes as long as a page timeout to answer Connect(), and the answer is
# worth waiting for - it says whether the device was there at all.
CONNECT_TIMEOUT_MS = 30000


def carries_audio(system, path):
    """Is this a device that could carry sound?

    The opposite of the pause watcher's question, and so the opposite default:
    there, doubt means pause, because the cost of not pausing is a podcast on
    a loudspeaker. Here, doubt means leave it alone - the cost of dialling
    something that is not a headset is paging a car in a car park.
    """
    try:
        uuids = system.call_sync(
            "org.bluez", path, "org.freedesktop.DBus.Properties", "Get",
            GLib.Variant("(ss)", ("org.bluez.Device1", "UUIDs")),
            GLib.VariantType("(v)"), Gio.DBusCallFlags.NONE, 1000, None,
        ).unpack()[0]
    except GLib.Error as err:
        print("could not read what %s is: %s - leaving it alone"
              % (path.rsplit("/", 1)[-1], err), flush=True)
        return False
    if not isinstance(uuids, (list, tuple)):
        return False
    return any(str(u)[:8].lower() in AUDIO_UUID_PREFIXES for u in uuids)


def is_trusted(system, path):
    """Has this device been trusted to connect without being asked?

    Trusted is BlueZ's own word for "this may connect whenever it likes", set
    when the device was paired from the phone. Dialling an untrusted device
    would be deciding something the pairing deliberately left open.
    """
    try:
        return bool(system.call_sync(
            "org.bluez", path, "org.freedesktop.DBus.Properties", "Get",
            GLib.Variant("(ss)", ("org.bluez.Device1", "Trusted")),
            GLib.VariantType("(v)"), Gio.DBusCallFlags.NONE, 1000, None,
        ).unpack()[0])
    except GLib.Error as err:
        print("could not read whether %s is trusted: %s - leaving it alone"
              % (path.rsplit("/", 1)[-1], err), flush=True)
        return False


def in_a_call(session):
    """Is callaudiod switching the card for a call right now?

    callaudiod is the most fragile thing in this stack - it looks a card up
    once and keeps the index, and a card appearing underneath it mid-call is
    how a call ends up silent in both directions. A headset is not worth that:
    whatever is being dialled here can wait until the call is over.

    Unreadable counts as "in a call": a callaudiod that is there but not
    answering is exactly the state this is meant to stay out of. Not being
    there at all is different and has to be told apart, or a phone without
    callaudiod would never reconnect anything and never say why.
    """
    try:
        mode = session.call_sync(
            "org.mobian_project.CallAudio", "/org/mobian_project/CallAudio",
            "org.freedesktop.DBus.Properties", "Get",
            GLib.Variant("(ss)", ("org.mobian_project.CallAudio",
                                  "AudioMode")),
            GLib.VariantType("(v)"), Gio.DBusCallFlags.NONE, 2000, None,
        ).unpack()[0]
    except GLib.Error as err:
        if (Gio.DBusError.is_remote_error(err)
                and Gio.DBusError.get_remote_error(err)
                == "org.freedesktop.DBus.Error.ServiceUnknown"):
            return False    # no callaudiod, so no call to get in the way
        print("callaudiod did not answer (%s) - not dialling now"
              % err.message, flush=True)
        return True
    return mode != 0


class Candidate:
    """The one device this service may dial, and what is owed to it.

    There is at most one. Two sets of earbuds fighting over the same output
    is not a thing to build on purpose, and the second one to disconnect is
    the one the phone was last using anyway.
    """

    def __init__(self, path, name):
        self.path = path
        self.name = name
        self.attempts_left = len(RETRY_DELAYS_S)
        self.busy = False          # a Connect() is out and unanswered
        self.timer = 0             # the pending GLib timeout, 0 for none
        self.connected = False     # it is on the air right now
        self.connected_at = 0.0    # monotonic, when it last came up
        self.seen_at = GLib.get_monotonic_time() / 1e6

    def cancel_timer(self):
        if self.timer:
            GLib.source_remove(self.timer)
            self.timer = 0


class Reconnector:
    """Holds the bus connections, the candidate, and the occasions.

    The connections are attributes and not locals, and that is not style. A
    subscription made on a connection that Python then frees is collected with
    it: the service stays running, the signal never arrives, and nothing is
    logged - the shape of a bug that cost this project two days in the
    killswitch indicator on 2026-09-14. Keep the connection, keep the signal.
    """

    def __init__(self, system, session):
        self.system = system
        self.session = session
        self.candidate = None
        self.last_attempt = 0.0    # monotonic, any attempt, any occasion
        # Is the phone in someone's hand? Read from logind at startup and
        # kept current by on_session_props. It decides only one thing: who
        # is likely to have hung up - see on_disconnected.
        self.active = True

    # -- dialling ---------------------------------------------------------

    def try_connect(self, why):
        """One attempt, asynchronously. True if one was actually started."""
        cand = self.candidate
        if cand is None or cand.busy:
            return False
        if cand.connected:
            return False    # already on the air; Connect() would only error
        if in_a_call(self.session):
            print("%s: not dialling %s during a call"
                  % (why, cand.name), flush=True)
            return False

        cand.busy = True
        self.last_attempt = GLib.get_monotonic_time() / 1e6
        print("%s: connecting %s" % (why, cand.name), flush=True)
        self.system.call(
            "org.bluez", cand.path, "org.bluez.Device1", "Connect", None,
            None, Gio.DBusCallFlags.NONE, CONNECT_TIMEOUT_MS, None,
            self.on_connect_done, cand,
        )
        return True

    def on_connect_done(self, conn, result, cand):
        cand.busy = False
        try:
            conn.call_finish(result)
        except GLib.Error as err:
            # Every failure mode lands here and they all mean the same thing
            # in practice - the device did not answer the page. Say which,
            # because "Host is down" (switched off or in its case) and
            # "Connection refused" (busy with another phone) are different
            # stories when somebody reads this later.
            print("could not connect %s: %s" % (cand.name, err.message),
                  flush=True)
            return
        # The Connected property arrives as its own signal and does the
        # bookkeeping; nothing to do here but say it worked.
        print("connected %s" % cand.name, flush=True)

    # -- the series after a disconnect ------------------------------------

    def schedule_next(self):
        cand = self.candidate
        if cand is None or cand.attempts_left <= 0:
            return
        delay = RETRY_DELAYS_S[len(RETRY_DELAYS_S) - cand.attempts_left]
        cand.timer = GLib.timeout_add_seconds(delay, self.on_timer)

    def on_timer(self):
        cand = self.candidate
        if cand is None:
            return False
        cand.timer = 0
        cand.attempts_left -= 1
        self.try_connect("retry")
        self.schedule_next()
        return False    # each delay is scheduled once, from the one before

    # -- occasions --------------------------------------------------------

    def on_bluez_props(self, _conn, _sender, path, _iface, _signal, params):
        iface, changed, _invalidated = params.unpack()
        if iface != "org.bluez.Device1":
            return
        connected = changed.get("Connected")
        if connected is True:
            self.on_connected(path)
        elif connected is False:
            self.on_disconnected(path)

    def on_connected(self, path):
        """Something connected. If it is what we were dialling, we are done."""
        cand = self.candidate
        if cand is None or cand.path != path:
            return
        cand.cancel_timer()
        cand.connected = True
        cand.connected_at = GLib.get_monotonic_time() / 1e6
        cand.attempts_left = 0

    def on_disconnected(self, path):
        name = path.rsplit("/", 1)[-1]

        # It came back and then left again straight away: multipoint earbuds
        # handing themselves to something else. Dialling into that is a tug
        # of war, so this one is left until the next time the phone wakes up.
        cand = self.candidate
        if (cand is not None and cand.path == path and cand.connected_at
                and GLib.get_monotonic_time() / 1e6 - cand.connected_at
                < KEPT_S):
            print("%s dropped us again after a moment - leaving it to settle"
                  % name, flush=True)
            cand.cancel_timer()
            cand.attempts_left = 0
            cand.connected = False
            cand.connected_at = 0.0
            return

        if not carries_audio(self.system, path):
            return
        if not is_trusted(self.system, path):
            print("%s disconnected, but it is not trusted - leaving it alone"
                  % name, flush=True)
            return

        if cand is not None:
            cand.cancel_timer()
        self.candidate = Candidate(path, name)

        # Who hung up? There is no way to ask - BlueZ does not pass the
        # disconnect reason to D-Bus - but there is a good proxy. If the phone
        # was in your hand when it happened, it was probably you, in the
        # settings, and dialling straight back would be this service arguing
        # with its owner. If the phone was idle or locked, nobody here did
        # anything, and the earbuds went flat or went into their case.
        #
        # Either way the device is remembered, so waking up will still try.
        if self.active:
            print("%s disconnected while the phone was in use - remembering "
                  "it, but not dialling back" % name, flush=True)
            return

        print("%s disconnected" % name, flush=True)
        self.schedule_next()

    def on_removed(self, _conn, _sender, _path, _iface, _signal, params):
        """The device was unpaired or forgotten: forget it here too."""
        path, _ifaces = params.unpack()
        cand = self.candidate
        if cand is not None and cand.path == path:
            cand.cancel_timer()
            self.candidate = None
            print("%s is gone from BlueZ - nothing to reconnect"
                  % cand.name, flush=True)

    def on_session_props(self, _conn, _sender, _path, _iface, _signal,
                         params):
        """logind: the phone went idle or locked, or came back.

        Both keys are watched because both are real and they do not arrive
        together - IdleHint comes first, when the screen goes off, LockedHint
        only when the lock actually takes. Measured 2026-09-14.
        """
        _iface_name, changed, _invalidated = params.unpack()
        hints = [changed[k] for k in ("IdleHint", "LockedHint")
                 if k in changed]
        if not hints:
            return
        was_active = self.active
        self.active = not any(hints)
        if self.active and not was_active:
            self.on_wakeup()

    def on_wakeup(self):
        """The phone is back in someone's hand. One attempt, if one is owed.

        Rate-limited, and that limit is the whole reason this is safe to leave
        running. A phone is unlocked dozens of times a day, and IdleHint goes
        false for reasons that have nothing to do with anyone touching it -
        measured on this phone the first time the service ran. Without the
        gap, this occasion would be a poller with extra steps.
        """
        cand = self.candidate
        if cand is None:
            return
        now = GLib.get_monotonic_time() / 1e6
        if now - cand.seen_at > CANDIDATE_TTL_S:
            print("%s has been gone for a day - not dialling it any more"
                  % cand.name, flush=True)
            cand.cancel_timer()
            self.candidate = None
            return
        if self.last_attempt and now - self.last_attempt < WAKE_MIN_GAP_S:
            return
        cand.cancel_timer()
        cand.attempts_left = 0     # the series, if any, is over
        self.try_connect("phone back in use")


def session_path(system):
    """This user's logind session object, or None if it cannot be had.

    Without it there is no waking-up occasion and only the series after a
    disconnect survives - worth saying out loud rather than failing, because
    the service is still useful with one occasion instead of two.
    """
    try:
        return system.call_sync(
            "org.freedesktop.login1", "/org/freedesktop/login1",
            "org.freedesktop.login1.Manager", "GetSession",
            GLib.Variant("(s)", ("auto",)), GLib.VariantType("(o)"),
            Gio.DBusCallFlags.NONE, 2000, None,
        ).unpack()[0]
    except GLib.Error as err:
        print("no logind session (%s) - waking the phone will not "
              "reconnect anything" % err.message, flush=True)
        return None


def session_in_use(system, path):
    """Is the phone being used right now, per logind?

    Read once at startup, because the signals only ever say what CHANGED. A
    service that starts while the screen is off and assumes otherwise sees the
    next unlock as no change at all, and the waking-up occasion goes missing
    until the screen has gone off once more - which on a phone that is put
    down and picked up all day is a long time to be wrong.
    """
    def hint(key):
        try:
            return bool(system.call_sync(
                "org.freedesktop.login1", path,
                "org.freedesktop.DBus.Properties", "Get",
                GLib.Variant("(ss)", ("org.freedesktop.login1.Session", key)),
                GLib.VariantType("(v)"), Gio.DBusCallFlags.NONE, 2000, None,
            ).unpack()[0])
        except GLib.Error:
            return False
    return not (hint("IdleHint") or hint("LockedHint"))


def main():
    system = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    session = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    rc = Reconnector(system, session)

    system.signal_subscribe(
        "org.bluez", "org.freedesktop.DBus.Properties", "PropertiesChanged",
        None, None, Gio.DBusSignalFlags.NONE, rc.on_bluez_props,
    )
    system.signal_subscribe(
        "org.bluez", "org.freedesktop.DBus.ObjectManager",
        "InterfacesRemoved", None, None, Gio.DBusSignalFlags.NONE,
        rc.on_removed,
    )
    path = session_path(system)
    if path is not None:
        rc.active = session_in_use(system, path)
        system.signal_subscribe(
            "org.freedesktop.login1", "org.freedesktop.DBus.Properties",
            "PropertiesChanged", path, None, Gio.DBusSignalFlags.NONE,
            rc.on_session_props,
        )

    print("watching for a Bluetooth audio device to dial back", flush=True)
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
