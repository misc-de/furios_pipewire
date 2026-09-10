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

**The VoIP nodes never become the default.** Low priority is not enough:
WirePlumber keeps earlier choices as a fallback chain and walks it when a
device disappears. Unplugging a headset mid playback left the default on
`droid-voip-sink` - which feeds the HAL's voice path and does not reach the
speaker, so the phone simply went quiet. `droid-default-sink-policy.lua` puts
it back.

**The microphone deliberately stays on the phone.** The BT card offers a source
and it reads happily - 192000 bytes of pure silence, RMS 0, peak 0. It is a
loopback node that exists so that opening it triggers the headset profile, and
that path carries no audio here either.

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

**Reconnect the headset once after a profile switch.** audioctl restarts
WirePlumber; a device that was already connected before does not fully
re-register its profiles - the card then shows only part of them (say only HFP,
no A2DP). After `bluetoothctl disconnect` and `connect` both are there.

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
