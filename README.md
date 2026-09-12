# furios_pipewire

Audio on the FuriPhone (FuriOS, MediaTek mt6877) runs through
**PulseAudio + module-droid-card** out of the box; the PipeWire that ships with
it is limited to camera and screencast (`pipewire-droid.conf` loads no
`api.alsa.*`).

This repository contains two things:

1. **`audioctl`** - switching between audio profiles, reversible at any time.
2. **`poc/spa-droid/`** - an SPA plugin that connects the Android audio HAL
   directly to PipeWire, so PulseAudio can be dropped entirely.

**Two files:** this one says what it does and how to run it.
**[FINDINGS.md](FINDINGS.md)** says why it is built this way - every
measurement, every wrong turn, and the traps that cost days.

## Profiles (audioctl)

| Profile     | Meaning                                                          |
|-------------|------------------------------------------------------------------|
| `standard`  | shipped state: PulseAudio holds the HAL                          |
| `pw-tunnel` | PipeWire gets a sink via pulse-tunnel, PA stays HAL owner        |
| `pw-hal`    | PipeWire talks to the HAL directly through `libspa-droid`        |

    audioctl status
    audioctl try pw-hal     # only until reboot, falls back automatically
    audioctl set standard   # persistent

Safety net: `try` discards the profile on reboot, and if no sink exists 15 s
after the switch, audioctl falls back to `standard` on its own.
**`audioctl verify` only checks that a sink exists - not that audio flows.**
After a switch, actually play something.

Before switching to `pw-hal`, audioctl checks whether the plugin still matches
the installed PipeWire version (`built-against` next to the plugin file). If a
system update breaks the SPA interface, `pw-hal` would otherwise go silent
without a word - now there is a warning telling you to rebuild.

`audioctl rescue` makes sound audible again: shipped state, speaker instead of
earpiece, unmuted, 65 %. `bt-call` catches aborts with a trap so an interrupted
test does not leave anything silent.

On a switch audioctl also kills `callaudiod` and `feedbackd`. Both hold
persistent connections to the audio server and do not survive a server change:
callaudiod finds no card afterwards, feedbackd fails with "Invalid state" - the
ringtone stays off and only the vibration is left. D-Bus restarts both on
demand.

## Status

Playback, capture and telephony through PipeWire -> `libspa-droid` -> Android
HAL all work, including a real phone call (confirmed on the device 2026-09-09)
and Bluetooth calls over a headset (2026-09-12, heard end to end).

| | |
|---|---|
| playback, capture, route switching | works |
| phone call: ringtone, both directions, speaker button | works |
| Bluetooth music (A2DP, AAC/SBC-XQ) | works |
| Bluetooth call (HFP) | works - needs the codec announced, see FINDINGS |
| echo during a call | **open** |
| `deep_buffer` / `compress_offload` for lower power | **unused** |

The control chain telephony runs on:

| callaudiod            | card               | HAL                       |
|-----------------------|--------------------|---------------------------|
| `SelectMode(1)`       | profile `voicecall`| `AUDIO_MODE_IN_CALL`      |
| `EnableSpeaker(false)`| `output-earpiece`  | route `Earpiece`          |
| `EnableSpeaker(true)` | `output-speaker`   | route `Speaker`           |
| `SelectMode(0)`       | profile `default`  | `AUDIO_MODE_NORMAL`       |

**callaudiod is the fragile part of all of this** - it keeps one reference to
the card and waits synchronously for every PulseAudio operation it starts, so
anything that changes the set of cards underneath it leaves the next call
without audio in either direction. `audioctl` and
`furios-audio-callaudio-refresh.service` handle the cases we know of;
FINDINGS.md has the three that cost a day each.

## As a package

    ./packaging/build-deb.sh
    sudo dpkg -i packaging/furios-audio-pipewire_*.deb

The package ships everything: the plugin, the WirePlumber monitor and
configuration, audioctl, the switcher app with its icon, the systemd units. It
installs into `/usr`, the scripts below into `/usr/local` - audioctl finds its
files in both paths.

The dependency on `pipewire (>= X, << X+1)` is deliberate: the plugin is built
against a specific SPA interface. If an update breaks it the sound would
otherwise disappear without a word - this way apt holds the package back, and
`audioctl` warns before switching on top of that.

Remove with `sudo dpkg -r furios-audio-pipewire`. If `pw-hal` is set
persistently at that point, the package warns beforehand - otherwise there
would be no sound after the next reboot.

## Building

The clone can live anywhere - every path in this repo is relative to the
scripts themselves, nothing assumes a particular directory.

The upstream sources are not versioned here:

    mkdir -p src && git clone https://github.com/FuriLabs/pulseaudio-modules-droid-modern \
        src/pulseaudio-modules-droid-modern     # tested with d0e2330

    meson setup poc/spa-droid/build poc/spa-droid
    ninja -C poc/spa-droid/build
    ./install-hal.sh        # needs sudo, does NOT change the active profile

If you already have those sources somewhere else, point at them instead of
cloning again:

    meson setup poc/spa-droid/build poc/spa-droid \
        -Ddroid_src=/path/to/pulseaudio-modules-droid-modern

Alternatively build the package (see above) - that is the way that survives a
system update.

`poc/spa-droid/tools/port-droid-util.py` reproducibly cuts out the functions in
`common/droid-util.c` that dereference PulseAudio graph objects. Run it again
after an upstream update; the script reports when its patterns no longer match.

## Tests

    ./tests/run-tests.sh
    ./tests/coverage.sh      # line coverage

What is checked is the *decisions*: how a port is ranked, what the card does
with the volume the graph hands it, when the safety net fires, whether the app
and `audioctl` still speak the same words. No framework - this has to run on
the phone, where every extra dependency is one more thing that can be missing
at the moment the tests matter most.

What is deliberately not checked is whether sound comes out. That needs the
HAL, a headset, a real call, and the way to establish it is to measure - play a
tone, record it, count distinct sample values - not to assert.

Everything reachable without hardware is at **100 %**:

| | |
|---|---|
| `droid-device.c` | 100 % of 481 lines |
| `droid-pcm.c` | 100 % of 968 |
| `compat/pa-audio.c` | 100 % of 87 |
| `compat/pa-compat.c` | 100 % of 77 |
| `compat/pa-containers.c` | 100 % of 172 |
| `wireplumber/droid.lua` | 100 % of 133 |
| `wireplumber/droid-bluetooth-call.lua` | 100 % of 140 |
| `wireplumber/droid-default-sink-policy.lua` | 100 % of 61 |
| `wireplumber/droid-input-follows-output.lua` | 100 % of 51 |
| `audioctl` | 100 % of 282 |
| `tools/furios-audio-callaudio-refresh` | 100 % of 9 |
| `tools/furios-audio-helper` | 100 % of 44 |
| `gui/furios-audio-switch.py` | 100 % of 383 |
| `tools/furios-audio-pause-on-disconnect.py` | 100 % of 61 |
| `gen-pipewire-hal-conf.py` | 100 % of 28 |

How the stubs work and why each suite is built the way it is:
**[FINDINGS.md](FINDINGS.md#how-the-tests-are-built-and-why)**.

## Structure

`libspa-droid.so` exports three factories:

- **`api.droid.device`** - the card. Reads the Android file
  `audio_policy_configuration.xml`, reports profiles and routes and creates the
  two nodes. The routes are named exactly like PulseAudio's droid-card
  (`output-earpiece`, `output-speaker`, `input-builtin_mic`), because
  callaudiod looks for those names.
- **`api.droid.pcm`** - the sink. `process()` writes into a ring buffer, a
  separate `writer_thread` calls the HAL, because `pa_droid_stream_write`
  blocks and therefore must not run on the graph thread.
- **`api.droid.pcm.source`** - capture, the same thing backwards: a
  `reader_thread` calls the blocking `pa_droid_stream_read`.

`wireplumber/droid.lua` loads the device. The detour through WirePlumber is
necessary: if PipeWire instantiates the device directly from `context.objects`,
the result is raw SPA nodes without an adapter - those have no format
conversion and never show up in pipewire-pulse as a sink.

### The path of a route change

    pactl set-sink-port droid-sink output-earpiece
      -> device (in WirePlumber): set_param(Route), remembers the route
      -> SERIAL bit flips -> event device-params-changed
      -> droid.lua passes the route name to the node as SPA_PROP_params
      -> node (in the PipeWire daemon): pa_droid_stream_set_route()

The detour is necessary because device and node run in **different processes** -
only the node holds the HAL stream.

## The one thing that needs root

Switching the stack masks system units in `/etc/systemd/user` and writes one
systemd drop-in. Everything else `audioctl` does runs as the user.

That part used to be `sudo ln`, `sudo rm`, `sudo mv` and `sudo tee` straight
out of `audioctl` - root for arbitrary commands with arbitrary arguments. It is
now `/usr/libexec/furios-audio-helper`, which knows five operations and takes
no paths from its caller at all: the unit names are checked against a list and
every path is fixed in the helper. Masking is a symlink to `/dev/null`, and
doing that to the wrong unit is how a phone stops booting.

`pkexec` authenticates the caller against `de.furios.audioctl.configure`
(`auth_self_keep`: your own password, remembered for a few minutes, so a switch
asks once rather than once per step). This matters more than it looks, because
FuriOS ships

    furios ALL=(ALL) NOPASSWD:ALL

in `/etc/sudoers` - the whole user has passwordless root, so `sudo` would never
ask for anything. polkit does not go through sudo and is unaffected by that
line.

**The switcher app brings its own password dialog.** polkit asks whatever
authentication agent the session registered, and phosh registers none - so on
this phone every polkit action fails with "No authentication agent found",
ours included. `PasswordAgent` in `gui/furios-audio-switch.py` registers one
while the app is open, and unregisters it on shutdown. In a terminal `pkexec`
brings its own prompt, so `audioctl` works there either way; if neither is
available the app says so in a toast rather than letting a switch fail with
nothing on screen.

**The safety net at boot does not ask.** `furios-audio-apply.service` drops a
test profile before the sound stack starts, and nobody is there to type a
password - a safety net that stops to ask for one is not a safety net. It sets
`AUDIOCTL_NONINTERACTIVE=1`, and `audioctl` then uses `sudo`, which that
sudoers line lets through.

## Switching at the push of a button

    ./gui/install.sh

Installs a small GTK4/libadwaita switcher with its icon and launcher entry
("Audio Switch" in the app grid). One switch for the stack, underneath it what
is actually running, and a **Restore sound** button - that returns to the
shipped state, switches to the speaker, unmutes and sets an audible volume.
Exactly the combination that looks like "nothing works any more" after a failed
test.

By default the switcher does **not** remember the choice: a reboot returns to
the shipped state. If you want otherwise, flip the second switch first.

While switching, a **pulsing progress bar** shows what audioctl is currently
reporting - the lines are read while the program is still running. Deliberately
without a percentage: nobody knows in advance how long it takes, because
audioctl waits up to 15 seconds for a sink. An invented number stuck at 90 %
would be worse than none at all.

## Bluetooth

`libspa-0.2-bluetooth` has to be installed - without it there is no Bluetooth
audio at all, and WirePlumber only says "BlueZ SPA plugin is missing or
broken".

**Music (A2DP) and calls (HFP) both work.** The codec costs real power, and
the choice is yours: AAC is 7.3 % of a core while plain SBC is 4.3 %.

    pactl set-card-profile bluez_card.<MAC> a2dp-sink        # AAC
    pactl set-card-profile bluez_card.<MAC> a2dp-sink-sbc    # ~40 % cheaper

`tools/build-bluez5-aac.sh` builds the AAC encoder module Debian leaves out.

**Moving a call onto a connected headset is built and off by default:**

    wpctl settings -s furios.bluetooth-call-routing true

`droid-bluetooth-call.lua` then puts the headset into a hands-free profile,
tells the nodes which codec was negotiated, and routes the card to
`output-bluetooth_sco` - in that order, because the HAL reads the codec when it
opens the stream. It gives up and hands the call back to the phone rather than
fight callaudiod for the route.

**When a headset disconnects, playback pauses** instead of moving to the
loudspeaker (`furios-audio-pause-on-disconnect`). Only players with MPRIS can
be paused this way; that is a real limit and not papered over with a mute.

Why any of this is the way it is - the charging case, the commented-out audio
policy, SCO that does not cross HCI, and the codec that has to be announced -
is in **[FINDINGS.md](FINDINGS.md#bluetooth)**.

## Testing without installing

    ninja -C poc/spa-droid/build
    # build/spa-merged/: symlinks to all system SPA plugins plus our droid/
    SPA_PLUGIN_DIR=.../spa-merged pipewire -c poc/spa-droid/test-crash.conf
    PIPEWIRE_REMOTE=pipewire-droidtest pw-cli ls Node

`spa-inspect` is **not** suitable for this: it dlopen's its argument directly
and never consults `SPA_PLUGIN_DIR`.

## Licence and credits

The source files in this repository are **MIT** (see `LICENSE`), and every file
we wrote carries an SPDX header saying so. `NOTICE` states the short version of
what follows.

**The built plugin is not.** `libspa-droid.so` compiles six source files from
*pulseaudio-modules-droid-modern* straight into itself — `droid-util.c`,
`droid-config.c`, `config-parser-xml.c`, `conversion.c`, `sllist.c`,
`utils.c` — and those are **LGPL-2.1** (Copyright © 2013-2022 Jolla Ltd.),
version 2.1 only, not "or later". The resulting binary is a combined work, so
anyone distributing it — including the `.deb` from `packaging/build-deb.sh` —
distributes it under the LGPL-2.1 and owes its recipients the corresponding
source. Since everything here is public and `tools/port-droid-util.py`
reproduces the adaptation from an unmodified upstream checkout, that obligation
is easy to meet: point at this repository and at the upstream commit.

MIT on our side is deliberate: it imposes nothing on anyone who takes only
these files (say the SPA node, the WirePlumber scripts or `audioctl`) without
the Jolla code.

### What we build against

| Component | Licence | How it is used |
|---|---|---|
| [pulseaudio-modules-droid-modern](https://github.com/FuriLabs/pulseaudio-modules-droid-modern) | LGPL-2.1 | six source files compiled into the plugin |
| [PipeWire / SPA](https://pipewire.org) headers | MIT | the plugin interface itself |
| [WirePlumber](https://pipewire.pages.freedesktop.org/wireplumber/) Lua API | MIT | the monitor and the policy script |
| libpulse headers (`pulse/*.h`) | LGPL-2.1+ | types the ported code expects; the code behind them is ours (`compat/pa-audio.c`) |
| [libhybris](https://github.com/libhybris/libhybris) / Android headers | Apache-2.0 | `hardware/audio.h`, the HAL interface |
| [expat](https://libexpat.github.io/) | MIT | parsing `audio_policy_configuration.xml` |
| [pipewire-config-droid](https://github.com/furilabs/pipewire-config-droid) | BSD-3-Clause | `poc/spa-droid/test-crash.conf` is derived from its `pipewire-droid.conf` |
| GTK4 / [libadwaita](https://gitlab.gnome.org/GNOME/libadwaita) | LGPL-2.1+ | the switcher app, through PyGObject |

The headers under `poc/spa-droid/compat/pulsecore/` re-declare PulseAudio
interfaces so the ported code compiles — the names and signatures are dictated
by PulseAudio, the implementations behind them are ours.

### Projects we learned from

Not all of these contribute code; several were simply the reason something
works at all, and it took reading them to find out why.

- **[Jolla / Sailfish OS](https://github.com/mer-hybris/pulseaudio-modules-droid)** —
  the original droid modules. Every hard-won detail about the Android HAL in
  this repo (routing by device type, the primary-stream assertion, the
  `voip_rx` rate override) was learned from that code.
- **[Droidian](https://github.com/droidian/pulseaudio-modules-droid-modern)** —
  carried the modules forward to Android 11 HALs.
- **[FuriLabs / FuriOS](https://github.com/FuriLabs)** — the distribution this
  runs on; `pipewire-hal.conf` is generated from their `pipewire-droid.conf`
  rather than replacing it.
- **[callaudiod](https://gitlab.com/mobian1/callaudiod)** (Mobian) — decides
  what a phone call sounds like. Our card reports `device.api = "droid-hal"`
  and names its profiles `default`/`voicecall` because callaudiod looks for
  exactly that; reading its droid path is what made telephony work.
- **[feedbackd](https://source.puri.sm/Librem5/feedbackd)** (Purism) — owns the
  ringtone. `audioctl` restarts it on a profile switch for that reason.
- **[oFono](https://git.kernel.org/pub/scm/network/ofono/ofono.git)** and
  **[BlueZ](http://www.bluez.org/)** — the HFP investigation in
  `51-bluez-ofono.conf` is a summary of what those two do on this device.
- **[Android Open Source Project](https://source.android.com)** — the audio HAL
  interface and its `audio_policy_configuration.xml`.
- **[PipeWire documentation on SPA plugins](https://docs.pipewire.org/page_spa_plugins.html)** —
  the map for everything under `poc/spa-droid/src/`.
