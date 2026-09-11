# furios_pipewire

Audio on the FuriPhone (FuriOS, MediaTek mt6877) runs through
**PulseAudio + module-droid-card** out of the box; the PipeWire that ships with
it is limited to camera and screencast (`pipewire-droid.conf` loads no
`api.alsa.*`).

This repository contains two things:

1. **`audioctl`** - switching between audio profiles, reversible at any time.
2. **`poc/spa-droid/`** - an SPA plugin that connects the Android audio HAL
   directly to PipeWire, so PulseAudio can be dropped entirely.

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

Playback and capture through PipeWire -> `libspa-droid` -> Android HAL work, as
does switching between speaker, earpiece and headset. The telephony control
chain is complete:

| callaudiod            | card               | HAL                       |
|-----------------------|--------------------|---------------------------|
| `SelectMode(1)`       | profile `voicecall`| `AUDIO_MODE_IN_CALL`      |
| `EnableSpeaker(false)`| `output-earpiece`  | route `Earpiece`          |
| `EnableSpeaker(true)` | `output-speaker`   | route `Speaker`           |
| `SelectMode(0)`       | profile `default`  | `AUDIO_MODE_NORMAL`       |

**Confirmed on the device (2026-09-09)**: a real phone call in the `pw-hal`
profile - ringtone audible, both sides hear each other, the speaker button
switches audibly, media playback unchanged afterwards. The journal excerpt:

    19:25:25  gnome-calls starts callaudiod
    19:25:29  profile voicecall -> AUDIO_MODE_IN_CALL -> route Earpiece at the HAL
    19:25:41  speaker button -> route Speaker at the HAL
    19:25:48  back to earpiece -> route Earpiece at the HAL
    19:25:50  hang up -> profile default -> AUDIO_MODE_NORMAL

For callaudiod to do this, three things have to be right, and every one of them
cost time:

- The sink has to report `device.api = "droid-hal"` (the identifier of
  PulseAudio's droid module). Only then does callaudiod take its droid path and
  switch between `default` and `voicecall`; with `droid` it falls back to the
  ALSA UCM path, looks for profiles prefixed `HiFi`/`Voice Call` and does
  nothing ("set_card_profile: nothing to be done").
- The profiles have to be named exactly `default` and `voicecall`.
- Wired ports have to be reported as **unavailable**. With droid cards
  callaudiod otherwise grabs the headset immediately - during a call the audio
  ended up nowhere instead of on the earpiece. PulseAudio reports them as
  unavailable on this device as well; there is no jack detection here
  (`/sys/class/extcon` only lists USB).

**Everything callaudiod touches is fragile in the same way.** It keeps one
reference to the card and waits, synchronously, for each PulseAudio operation
it starts. Anything that changes the set of cards or nodes underneath it -
WirePlumber restarting, a profile switch, a Bluetooth card appearing or
changing profile - leaves its next `SelectMode` waiting for a completion that
never comes. The caller gives up after the D-Bus timeout of 25 seconds,
gnome-calls logs `Failed to select audio mode: Timeout was reached`, and the
call has no audio in either direction: the card never reaches the `voicecall`
profile, so the HAL never leaves `AUDIO_MODE_NORMAL` and there is no voice
path to hear. Picking a device by hand does not help, because it is the mode
that is missing, not the device. Three of these on one day, each with a
different trigger, are the reason for the three paragraphs below. Confirmed
working again on 2026-09-11 after them.

**callaudiod has to be restarted whenever the card is.** It looks the card up
once and keeps its index; a WirePlumber restart destroys every device and
creates it again with a new one - measured: card 161 before, 248 after.
callaudiod sees the new sinks and sources and never re-reads the card list, so
the next call stops here:

    Change mode from '0', to '1'
    card has voice profile, using it
    <nothing - 25 seconds, then the D-Bus timeout>

and the call has no audio in either direction, because the HAL never left
`AUDIO_MODE_NORMAL`. A profile switch takes care of this in `audioctl`;
`furios-audio-callaudio-refresh.service` covers every other way WirePlumber
comes back - an upgrade, a crash, someone typing `systemctl`. It leaves
callaudiod alone while a call is running: a restart at that moment has broken
the call anyway, and taking callaudiod away would leave the card in the
`voicecall` profile with nobody to bring it back.

**The first call after a profile switch used to have no audio at all**, and
the reason was not in the sound path. Switching profiles kills callaudiod, so
the next call starts it again through D-Bus - and when `SelectMode` is the
method that activates it, the call arrives while callaudiod is still bringing
its PulseAudio connection up. It blocks until the D-Bus timeout: 25 seconds,
two tries out of two against this stack, never against the shipped one.
gnome-calls logs `Failed to select audio mode: Timeout was reached`, the card
never reaches the `voicecall` profile, the HAL never leaves
`AUDIO_MODE_NORMAL`, and neither side hears anything - not on a headset, not
on the earpiece, and picking a device by hand changes nothing because the mode
is what is missing. `audioctl` now starts callaudiod itself after a switch,
with a method that asks nothing of it, and `SelectMode` then answers in about
a second.

**And nothing may pull the cards around while a call is being set up.** That
is the third trigger, and it was self-inflicted: `droid-bluetooth-call.lua`
switched the headset into its hands-free profile the moment the call started,
which is exactly when callaudiod is working through its own sequence. It now
waits 1.5 s before touching the Bluetooth card - callaudiod's sequence takes
about 200 ms - and the automatic routing is off by default until a real call
has shown that the whole thing works. See **Bluetooth** below.

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

What is checked is the *decisions*: how a port is ranked, what the card does
with the volume the graph hands it, when the safety net fires, whether the app
and `audioctl` still speak the same words. No framework - this has to run on
the phone, where every extra dependency is one more thing that can be missing
at the moment the tests matter most.

What is deliberately not checked is whether sound comes out. That needs the
HAL, a headset, a real call, and the way to establish it is to measure - play a
tone, record it, count distinct sample values - not to assert. The tests exist
so that the things which *can* be decided at a desk stop being rediscovered by
ear.

    ./tests/coverage.sh

reports line coverage. Everything that can be reached without hardware is at
**100 %**:

| | |
|---|---|
| `droid-device.c` | 100 % of 481 lines |
| `droid-pcm.c` | 100 % of 887 |
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

The Python is measured with the standard library's `trace` module, and runs
against `tests/gi_stub.py` - a stand-in for PyGObject, because both files start
with `import gi` and pulling in GTK, libadwaita and a main loop says nothing
about whether the code is right. The stub fabricates whatever is asked of it
and remembers how it was called, so a test can look at which widget was built
and with what. It proves the parts that decide things decide them right; it
proves nothing about GTK.

Two suites do not measure code but names and decisions around it.
`tests/test-wireplumber-conf.sh` checks every setting in `wireplumber/*.conf`
against WirePlumber's schema - or against the schema we declare ourselves in
the same file - and every `monitor.bluez.properties` key against the strings in
libspa-bluez5, because a name neither of them knows is ignored rather than
refused. `tests/test-callaudio-refresh.sh` covers the small script that gives
callaudiod a fresh view of the card: never during a call, stop it when its card
is gone, and start it again with a method that asks nothing of it - starting it
with `SelectMode` blocks for the same 25 seconds.

`audioctl` is measured with bash's own tracing: `PS4` carries `LINENO`, `set -x`
prints it, and what is left is arithmetic. It runs against a `PATH` where
`pactl`, `systemctl` and `sudo` are scripts that answer whatever the case under
test needs, and `--dry-run` wherever it would change something - `run()` then
prints the command instead of running it, which is exactly the seam a test
wants.

The WirePlumber scripts run inside a session manager, so `tests/lua/` gives
them one: a stub shallow enough to read in a sitting, where object managers
return what a test put there and every call the script makes is recorded, so a
test can look at what it did rather than at what it said. Coverage comes from
Lua's own line hook rather than a tool - same reason as the rest of the suite,
this has to run on the phone. A script that passes against the stub is one
whose logic holds, not one that is known to work on the device.

The card is started the way the daemon starts it - init, listen, enumerate,
set, sync, clear - against `tests/audio-policy-fixture.xml` rather than the
phone's own configuration, so the tests say something about the code and not
about one vendor's file. Two more fixtures cover what a bad file does: one with
no `primary` module, one with more ports than the card has room for. No HAL is
opened: the device parses XML and hands out parameters, and it is the node that
would touch hardware.

Getting the last lines took two decisions worth recording. A port with no
PulseAudio name has to be left out, but no vendor file can produce one - every
device type the parser understands has a name, and one it does not understand
is dropped before the card sees it. That test builds the module in memory
instead. And a `snprintf` whose return value cannot be negative had a branch
for the case that it is; counting a negative as nothing written keeps the same
protection without a line no test could reach. Removing a check to please a
coverage number would have been the other way to get there, and the wrong one.

`droid-pcm.c` is the half that touches hardware, and it is tested against a
stand-in: `tests/hal-stub.c` answers where the Android HAL would, counting the
bytes it is given, handing back what a test told it to hand back, and failing
on demand. The test links only the configuration half of the vendor code and
replaces the rest, which is why `meson.build` has a second static library for
it. What that proves is that the code *around* the HAL behaves - the ring
buffer, the give-up-after-three-refusals rule, the drain that would otherwise
swallow the last 21 ms of a song, the latency it reports, the audio source a
call takes away and never gives back. It proves nothing about the HAL itself,
and no stub ever will.

Two allocations and a thread that will not start cannot be provoked on a desk,
so the test redirects `malloc` and `pthread_create` for the one file it
includes - by macro, ending at an `#undef`, not by interposing on the process.
An allocation fails only at an exact size that nothing else asks for, so the
test's own output keeps working while the node's buffer comes back empty.

Three things in the node changed because the test asked what happens when they
fail. `pthread_create` returns its error rather than setting `errno`, so a
failed start read as a successful one; a timer that cannot be created or armed
left the node looking healthy and playing nothing. All three now say so.

Each test says which mistake it is there to prevent, because every one of them
was a real one: `auto_null` passing the safety net; a single stored channel
volume silencing the right channel of the microphone; renaming an `audioctl`
label and quietly breaking the app that reads it; a route published without
volume props, which makes every PulseAudio client see 0 % and drop what it
sets.

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

## Traps that cost time

**A configuration key that does not exist is not an error.** WirePlumber and
the SPA plugins read their configuration, find a name they have no schema or
handler for, and carry on with the default. The sound then behaves as if the
file were not there, and nothing says why. `bluez5.reconnect-profiles` sat in
`51-bluez-ofono.conf` for a day with a comment explaining what it did; it is a
PulseAudio name, and it exists nowhere in PipeWire, WirePlumber or
libspa-bluez5. The property does exist as `bluez5.auto-connect`, and the case
it is for is reproducible: leave the card in a hands-free profile, restart
WirePlumber, and without it only `off` and the two headset profiles remain
until someone disconnects and reconnects by hand; with it all three A2DP
profiles come back and the card picks `a2dp-sink` again on its own.
`tests/test-wireplumber-conf.sh` now checks the names in those files against
WirePlumber's own schema and the strings in libspa-bluez5.

**The package version sorts by commit count, not by commit hash.** dpkg
compares runs of digits numerically and everything else as text, so with
`0.1.0+git<date>.<hash>` it was the hash that decided the order between two
builds of the same day - and hashes are not monotonic. A dirty build of an
older commit outranked a clean newer one, and installing the newer package was
announced as a downgrade. `git rev-list --count HEAD` leads the version now;
date and hash follow, where they can be read but cannot affect the order.

- **A hardware sink has to drive the graph.** Without `spa_node_call_ready()`
  from its own timerfd PipeWire never calls `process()` - the sink appears but
  stays silent.
- **`spa_node_info.props` must not be NULL.** `module-adapter` passes it to
  `pw_properties_update` unchecked and segfaults.
- **Ports have to come from `hw->enabled_module`.** `pa_droid_hw_module_get`
  duplicates the configuration, `pa_droid_open_output_stream` compares by
  pointer identity - ports from your own copy are always rejected.
- **Without them, routing and level are missing after opening**: without
  `pa_droid_stream_set_route()` and `set_volume(1.0)` the stream stays silent.
- **Node tests without an adapter are incomplete** - `probe-droid-node` talks to
  the node directly and skips exactly the layer that crashed.
- **Param changes are announced via the SERIAL bit**, not via the neighbouring
  `user` field: `params[i].flags ^= SPA_PARAM_INFO_SERIAL`. Without that nobody
  learns about the new route - PipeWire keeps handing clients the old cached
  value, and WirePlumber's route policy overwrites the selection right away.
- **The buffer size is the graph quantum, not the HAL period.** On output both
  happen to be 4096 B, on input the HAL delivers 3840 B - too small a buffer
  makes libspa-audioconvert crash.
- **A card-backed node takes its volume from the active route.** That is where
  pipewire-pulse reads it - a card whose routes carry no volume reports 0 % to
  every PulseAudio client and silently drops what they set, while `wpctl` works
  fine. Reporting it has a price: PipeWire then stops applying the level in
  software, because it assumes the hardware does.
- **This HAL cannot attenuate.** It accepts `set_volume` on the primary output
  and returns success, but the level does not move: measured over the speaker,
  RMS 5796 at 100 % against 5734 at 20 %. On Android that gain lives in
  AudioFlinger, above the HAL. So `droid.lua` puts the level from the route
  back onto the node, where the graph applies it for real - measured again,
  RMS 5693 against 263 at 25 %.
- **The HAL keeps the microphone after a call unless you take it back.** When a
  call starts the HAL takes the mix port's audio source for itself
  ("overriding audio source mic with voice call") and does not return it when
  the call ends. Everything recording afterwards reads digital silence, until
  something restarts the audio stack - which is a long way from the symptom.
  The capture node now hears about the end of the call and sets its source
  again. Measured after hanging up: RMS 548 across 3304 distinct sample values,
  where before the fix it was RMS 0.0 and exactly one value.
- **The phone has two microphones; the ports reach only one of them.** Both
  holes are visible on the case, one at each end, and Android offers
  `input-builtin_mic` and `input-back_mic` accordingly. Selecting between them
  changes nothing: with the bottom-firing speaker playing, three runs through
  each port measured 2876-3011 RMS, under four percent apart, where a capsule
  at the far end of the phone would be several decibels down. The HAL chooses
  its capsule by audio source and mode, not by the device it is routed to. The
  descriptions say both parts of that - where the hole is, and that the second
  port is not a second signal - because a label promising a top microphone
  would be the honest-sounding kind of wrong.
- **Two input ports are not microphones, and both used to rank like one.**
  `Voice Call In` is the tap on the call itself - outside a call it delivers
  digital silence - and `Built-In Back Mic` is the second capsule the DSP pairs
  with the first for noise reduction. PulseAudio's droid-card gives all three
  the same priority of 200, which leaves the choice of default microphone to
  the order the ports happen to appear in the vendor's XML. They are 50 and 150
  here, so the real microphone cannot lose that coin toss. Both stay
  selectable; neither can be picked by accident.
- **A dead capture path and a quiet room are easy to tell apart.** Count the
  distinct sample values, not the level. A live microphone in a silent room
  still delivers thousands of them - thermal noise, quantisation, dither. One
  distinct value, and that value zero, means no signal path at all. That is how
  the headset microphone was ruled out: 96000 samples, one distinct value,
  against 4287 from the phone's own microphone in the same room.
- **The plugin must never be unloaded** (`-Wl,-z,nodelete`). PipeWire drops an
  SPA plugin as soon as nothing uses it, which happens on every WirePlumber
  restart. That takes libhybris' Android linker state with it, and building the
  nodes again re-initialises it inside a process that has since filled its
  address space - the linker does not get the region it wants and faults.
- **Load the HAL module early, and never let it go.** libhybris brings its own
  Android linker, and that linker wants a particular region of the address
  space. Opened late - after the Bluetooth codecs are in the process, say - it
  does not get it and faults inside `android_linker_init()`, taking the whole
  daemon down with it. Closing the last reference and opening it again faults
  the same way, which used to happen on every suspend/resume. The first node
  now loads the module while the process is still young and keeps it for good;
  the stream, which is the exclusive part, is still opened only when something
  plays.
- **`spa_log_info` is invisible below PipeWire's default log level.** Turn
  diagnostics on with:

      systemctl --user set-environment SPA_DROID_DIAG=1

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

## VoIP and microphone selection

The card offers four nodes: `droid-sink`/`droid-source` for everything normal
and `droid-voip-sink`/`droid-voip-source` for the HAL's VoIP path (mix ports
`voip_rx`/`voip_tx`). The latter have low priority - nothing ends up there by
accident. Their capture uses the Android audio source `voice communication`,
for which the HAL turns on its voice processing.

Two quirks that drew blood:

- **Routing only on the primary stream.** `pa_droid_stream_set_route()` checks
  that with an assertion and **aborts the whole process** if you try it on a
  different mix port. The VoIP node reproducibly took PipeWire down that way
  (SIGABRT). Routing applies to all open streams anyway - the primary one sets
  it.
- **16 kHz, not 48.** The ported code enforces exactly that for `voip_rx`
  ("Override voip_rx channel map (mono) and sample rate (16000)"). The node
  therefore follows `audio.rate` from its properties instead of stubbornly
  offering 48 kHz - otherwise it would write into it at three times the speed.

`wireplumber/droid-input-follows-output.lua` makes the microphone follow the
output selection when both ends belong to the same device. Without it the
microphone stays on the phone as soon as callaudiod has pinned sink and source
once.

## Echo during a call

Does the far end hear itself? Software does not help: in a cellular call the
voice path runs **modem <-> DSP**, not through the host - an echo canceller in
PipeWire or PulseAudio would never see the data. The cancellation lives in the
DSP and MediaTek calls it **DMNR** (dual-microphone noise and echo reduction).

On this device the vendor's tuning file says:

    MTK_DUAL_MIC_SUPPORT         yes   two microphones are present
    MTK_HANDSFREE_DMNR_SUPPORT   yes   the chip can do handsfree DMNR
    MTK_INCALL_HANDSFREE_DMNR    no    during a call it is disabled
    MTK_VOIP_HANDSFREE_DMNR      no
    MTK_VOIP_NORMAL_DMNR         no

There is a switch to try it - in the switcher app under "Call echo", or on the
command line:

    furios-audio-dmnr on       # mount the modified copy
    furios-audio-dmnr off      # back to the original
    furios-audio-dmnr status

It lays a modified copy over the file with a bind mount - the partition is
read-only and protected by dm-verity, and is left alone - and restarts the
audio stack (`audioctl restart`, the profile stays) so the HAL reads it. A
reboot of the device clears everything away. **Whether it helps is untested** -
only a real call can show that.

Two leads that turned out to be dead ends:

- `realcall=on`, which PulseAudio's card module sends for the call profile, is
  **rejected by this HAL** (`failed: -22`). Apparently a Qualcomm legacy. The
  code for it is there, the option is off.
- `speaker_before_voice=true` on the other hand is active: the HAL now briefly
  routes to the speaker before the mode change, as upstream recommends for
  devices that otherwise start a call wrongly.

## Bluetooth

For PipeWire to do Bluetooth audio, `libspa-0.2-bluetooth` has to be installed
- without that package there is no BT support at all, WirePlumber only reports
"BlueZ SPA plugin is missing or broken".

**A2DP (music) works. Hands-free (HFP) does not - on no stack, not even in the
shipped state.** Measured:

- ofono owns HFP: `hfp_ag_bluez5` is built in, `/bluetooth/profile/hfp_ag` is
  registered with BlueZ. Next to it PipeWire's native backend fails with
  `listen(): Address already in use` and
  `RegisterProfile() failed: NotPermitted`.
- ofono still creates no card for the connected headset
  (`HandsfreeAudioManager.GetCards` stays empty).
- On the shipped PulseAudio the result is the same: `handsfree_head_unit` can be
  selected, the source goes to `RUNNING` and delivers **0 bytes**; `paplay` in
  the other direction blocks.
- The HAL configuration knows BT SCO device ports, but neither our card nor
  PulseAudio's exposes them: on Android devices SCO runs in hardware between the
  BT chip and the audio DSP, not through the host.

`51-bluez-ofono.conf` therefore sets the **native** backend - not for audio
through the host, but because only then does the card offer a hands-free
profile at all, and only then does the headset establish an SCO channel. The
Android path needs that channel: the HAL puts the voice path onto the Bluetooth
PCM line via `BT_SCO=on`, and that line only carries while the connection is
up. See `audioctl bt-call`.

**`priority.session` has to be set on the node.** Without it WirePlumber's
device selection falls back to `priority.driver` - and that is 50000 here, so
the sink drives the graph. That makes the node beat even an explicit user
choice, which is weighted 30000: no other output device could be selected at
all, no Bluetooth headphones, nothing. The symptom is nasty because the
selection in the UI simply has no effect, without any error message.

**Only the Bluetooth roles a phone has.** `51-bluez-ofono.conf` sets
`bluez5.roles = [ a2dp_source hfp_ag hsp_ag ]`. The names are from this
device's point of view and the card profiles from the remote's, so the role
behind the profile called `a2dp-sink` (High Fidelity Playback) is
`a2dp_source`: we send, the headset receives. With the headset-side roles left
on, the card also offers `audio-gateway` - **priority 256 against a2dp-sink's
132, and no sinks or sources at all**. When a headset connects on its own the
handsfree side comes up first, WirePlumber picks the highest-priority profile
it can get, and that is the empty one; nothing re-evaluates when A2DP arrives
later. The headset then sits there connected and silent, with a tick next to it
in the settings.

**A Bluetooth speaker takes over when it connects.** Not by priority - the BT
sink ranks 1010 against the phone's 1000, which would be enough - but because
`droid-bluetooth-takes-over.lua` drops the configured default that callaudiod
pins on the phone card (`SET_DEFAULT_SINK`, weighted 30000, set on the first
call or ringtone and never taken back). The pin is dropped rather than
replaced, so the phone takes over again the moment the headset is gone. Never
during a call.

**Moving a call to a connected headset is built, and switched off.** On this
device the voice path of a mobile call never reaches the host - it runs modem
<-> DSP. The headset is served by the HAL, which puts the path onto the
Bluetooth PCM line when the card is routed to a BT SCO device.
`droid-bluetooth-call.lua` does the three things that have to happen together:
headset into a hands-free profile so an SCO channel exists, card onto
`output-bluetooth_sco` and `input-bluetooth_sco_headset`, and the route set
again whenever callaudiod moves it back to the earpiece. Everything is undone
when the call ends.

The dry run was convincing - this is what the HAL reports:

    hw set_parameters(BT_SCO=on)
    Created output audio patch "primary output"->"BT SCO"
    Audio mode AUDIO_MODE_IN_CALL, overriding audio source mic with voice call
    Created input audio patch "primary input"<-"BT SCO Headset Mic"

The microphone comes along on the same channel; there is nothing separate to
switch. And then the first real call had no audio at all - not on the headset,
not on the earpiece, in neither direction, and picking the earpiece by hand
changed nothing. Two things were wrong, both invisible to a dry run:

- **The route does not stay.** callaudiod only knows earpiece and speaker and
  resets the port during a call. The script set it back, callaudiod set it
  again, two to three round trips a second, and the HAL tore the voice path
  down and rebuilt it each time (`BT_SCO=off`, `BT_SCO=on`, ...).
- **Changing the Bluetooth profile mid-setup wedges callaudiod.** It changes
  the set of cards underneath a sequence of PulseAudio operations callaudiod
  is waiting on, and its next `SelectMode` then blocks until the D-Bus timeout
  - 25 seconds, no audio, or a hang-up that leaves the phone in the
  `voicecall` profile.

Both were fixed. The script gives up after three rounds of the route fight and
hands the call back to the phone - a call on the earpiece is a nuisance, a call
with no audio is not - and it no longer waits a fixed span before touching the
Bluetooth card but waits for the card to go **quiet**: every route or profile
change while the call is being set up arms the wait again, and only a second in
which nothing moves lets the takeover through. A fixed delay was tried first
and was not enough, because the profile goes to `voicecall` while the phone is
still *ringing* - the delay elapsed 14 ms before callaudiod set the port it
wanted. It also notices a headset connected *during* a call, which it could not
before: nothing moves on the phone card then, so the Bluetooth card announcing
itself is the only notice there is.

Measured on the third call, with all of that in place:

    14:49:17.175  profile voicecall
    14:49:17.573  callaudiod sets the earpiece
    14:49:18.625  takeover -> BT SCO          (+1.05 s of quiet)
    14:49:18.637  hw set_parameters(BT_SCO=on)
                  no route change for the rest of the call

Nothing fought, the state was exactly what it should be - and **there was no
audio in either direction**, on the headset or on the phone. Put next to the
other end of the same question, where the SCO channel delivers 48000 samples
with one distinct value to the host, that is the answer for this device:
**Bluetooth telephony does not work here, by any of the paths there are.** The
mobile voice path runs modem <-> DSP, the HAL will say it has put that path on
the Bluetooth line, and no sound comes out of it.

So it stays **off**. The script stays too, because none of it is wrong and
another device may well behave differently:

    wpctl settings -s furios.bluetooth-call-routing true

turns it on, `false` turns it off again. The setting is declared in
`51-bluez-ofono.conf`; WirePlumber ignores a setting it has no schema entry
for, silently, which is why the declaration is there and why
`tests/test-wireplumber-conf.sh` checks every name in those files.

**The host-side headset microphone delivers nothing**, and that is not a
configuration mistake: with the card in `headset-head-unit` and the native
backend, eight seconds of capture produced 64000 bytes in which every single
sample is zero. The SCO link does not carry audio to the host on this device -
the controller keeps it in hardware between the BT chip and the audio DSP. So
VoIP calls (Signal, SIP) over a headset cannot work here, while mobile calls
can, because those never needed the host in the first place.

**Playback pauses when Bluetooth disconnects.** Earbuds run out of battery, or
one goes back into its case, and without this the audio moves to the next best
output - which on a phone is the loudspeaker, in whatever room you are standing
in. `furios-audio-pause-on-disconnect` watches BlueZ and asks every MPRIS
player that is currently playing to pause, the way Android does. It leaves
nothing behind: no muted sink, no changed default, nothing to undo. A player
without MPRIS cannot be paused this way - papering over that with a mute would
be a trap of its own, since a phone that is silent for reasons nobody remembers
is worse than one that was briefly too loud.

**The VoIP nodes never become the default.** Low priority is not enough:
WirePlumber keeps earlier choices as a fallback chain and walks it when a
device disappears. Unplugging a headset mid playback left the default on
`droid-voip-sink` - which feeds the HAL's voice path and does not reach the
speaker, so the phone simply went quiet. `droid-default-sink-policy.lua` puts
it back.

**The microphone deliberately stays on the phone, and is no longer offered at
all.** The BT card offers a source and it reads happily - 192000 bytes of pure
silence, RMS 0, peak 0. It is a loopback node that exists so that opening it
triggers the headset profile, and that path carries no audio here either. It
is kept alive in every profile by WirePlumber's setting
`bluetooth.autoswitch-to-headset-profile`, whose own description is "Always
show microphone for Bluetooth headsets" - so every recorder listed the headset
as a microphone, picking it appeared to work, and the recording was silence.
Measured again on 2026-09-10, 3 s at 16 kHz mono: 48000 samples, **one**
distinct value, RMS 0, in the A2DP profile and in the hands-free profile
alike. The setting is off in `51-bluez-ofono.conf`; the loopback source now
exists only while the card really is in a hands-free profile, which is where a
call puts it. `droid-input-follows-output.lua` keeps its Bluetooth guard
anyway - the node can still appear, and the rule that the microphone must not
follow onto it has not changed.

**Bluetooth music quality is a codec question, and Debian is one codec short.**
`libspa-0.2-bluetooth` ships modules for SBC, LDAC, aptX, Opus and LC3 - but
not AAC, which needs fdk-aac. Most earbuds offer AAC and SBC and nothing else,
so without it PipeWire falls back to SBC, and the headset decides how good that
gets: the one measured here caps SBC at bitpool 39, which even SBC-XQ runs into.
`tools/build-bluez5-aac.sh` builds the missing module from the matching PipeWire
sources and drops it in beside the others - additive, replacing nothing dpkg
owns. `audioctl` warns when a PipeWire update has moved past it, because the
only symptom would otherwise be that Bluetooth music quietly sounds worse than
it did yesterday.

Reading what a headset actually offers, rather than guessing:

    busctl tree org.bluez | grep sep          # one entry per codec it offers
    busctl get-property org.bluez /org/bluez/hci0/dev_<MAC>/sep1 \
        org.bluez.MediaEndpoint1 Codec        # 0 = SBC, 2 = AAC
    busctl get-property org.bluez /org/bluez/hci0/dev_<MAC>/sep3 \
        org.bluez.MediaEndpoint1 Capabilities # SBC: 4th byte is the max bitpool

**A headset that was connected before a WirePlumber restart loses half its
profiles.** The card then shows only part of them - say the two headset
profiles and no A2DP at all, so music would play in mono at 16 kHz.
`bluetoothctl disconnect` and `connect` brings them back, which is not
something to ask of anyone; `bluez5.auto-connect` in `51-bluez-ofono.conf`
asks for the missing role instead. Measured both ways: without it only `off`
and the headset profiles survived a restart in hands-free, with it all three
A2DP profiles came back and the card picked `a2dp-sink` again on its own.

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
