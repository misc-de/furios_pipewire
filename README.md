# furios_pipewire

Lets PipeWire drive the audio hardware on the FuriPhone FLX1 directly, and
switches between that and the shipped setup at any time.

Out of the box, sound on this phone belongs to **PulseAudio**, which owns the
Android audio HAL; the PipeWire that ships alongside it is limited to camera
and screencast. That works, but it keeps the phone on a sound server the rest
of the desktop stack has moved away from.

This repository contains two things:

1. **`audioctl`** — switching between audio profiles, reversible at any time.
2. **`poc/spa-droid/`** — an SPA plugin that connects the Android audio HAL
   directly to PipeWire, so PulseAudio can be dropped entirely.

Playback, recording, phone calls and Bluetooth — music, calls and the headset
microphone — all work through it on the device.

## Install

    ./packaging/build-deb.sh
    sudo dpkg -i packaging/furios-audio-pipewire_*.deb

The package ships the plugin, the WirePlumber monitor and configuration,
`audioctl` and the systemd units. Remove it with
`sudo dpkg -r furios-audio-pipewire`.

The dependency on a specific PipeWire version is deliberate: the plugin is
built against one SPA interface, and an update that broke it would otherwise
take the sound with it silently.

## Usage

| Profile | What it means |
|---|---|
| `standard` | shipped state: PulseAudio owns the HAL |
| `pw-tunnel` | PipeWire gets a sink through PulseAudio, which stays the owner |
| `pw-hal` | PipeWire talks to the HAL directly |

    audioctl status              show the current state
    audioctl try <profile>       switch until the next reboot
    audioctl set <profile>       switch and remember it
    audioctl revert              back to standard immediately
    audioctl rescue              make sound audible again: shipped state,
                                 speaker, unmuted, 65 %
    audioctl bt-call watch       put a call on the Bluetooth headset and
                                 hold it there
    audioctl bt-mic on|off       record from the headset outside a call

Nothing here needs root.

There are two safety nets: `try` drops the profile at the next reboot, and if
no sink appears within 15 s of a switch, `audioctl` falls back to `standard` by
itself. Note that this checks whether a sink *exists*, not whether sound comes
out — **after switching, play something.**

## Building the plugin

The upstream sources are not versioned here:

    mkdir -p src && git clone https://github.com/FuriLabs/pulseaudio-modules-droid-modern \
        src/pulseaudio-modules-droid-modern

    meson setup poc/spa-droid/build poc/spa-droid
    ninja -C poc/spa-droid/build
    ./install-hal.sh        # needs sudo, does not change the active profile

Point `-Ddroid_src=` at an existing checkout to use one you already have.
Building the package instead is the way that survives a system update.

## Tests

    ./tests/run-tests.sh

Everything that can be decided at a desk: how a port is ranked, what the card
does with a volume, when the safety net fires. Whether sound actually comes out
is not something to assert — it has to be measured on the device.

## Licence

Our own code is MIT (see [LICENSE](LICENSE)). The built plugin compiles six
LGPL-2.1 source files from *pulseaudio-modules-droid-modern* into itself, so
**the plugin and the package are LGPL-2.1**. What this builds on, and under
which licence, is in [NOTICE](NOTICE).

Why it is built this way, and the measurements behind each decision, are in
[FINDINGS.md](FINDINGS.md).
