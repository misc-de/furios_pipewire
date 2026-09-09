# furios-audio

Audio auf dem FuriPhone (FuriOS, MediaTek mt6877) laeuft ab Werk ueber
**PulseAudio + module-droid-card**; das mitgelieferte PipeWire ist auf Kamera
und Screencast beschraenkt (`pipewire-droid.conf` laedt kein `api.alsa.*`).

Dieses Verzeichnis enthaelt zweierlei:

1. **`audioctl`** - Umschaltung zwischen Audio-Profilen, jederzeit reversibel.
2. **`poc/spa-droid/`** - ein SPA-Plugin, das den Android-Audio-HAL direkt an
   PipeWire anbindet, damit PulseAudio ganz entfallen kann.

## Profile (audioctl)

| Profil      | Bedeutung                                                        |
|-------------|------------------------------------------------------------------|
| `standard`  | Auslieferungszustand: PulseAudio haelt den HAL                    |
| `pw-tunnel` | PipeWire bekommt einen Sink via pulse-tunnel, PA bleibt HAL-Herr  |
| `pw-hal`    | PipeWire spricht ueber `libspa-droid` direkt mit dem HAL          |

    audioctl status
    audioctl try pw-hal     # nur bis zum Neustart, faellt automatisch zurueck
    audioctl set standard   # persistent

Sicherheitsnetz: `try` verwirft das Profil beim Neustart, und fehlt 15 s nach
dem Wechsel ein Sink, schaltet audioctl von selbst auf `standard` zurueck.
**`audioctl verify` prueft nur, ob ein Sink existiert - nicht, ob Ton fliesst.**
Nach einem Wechsel wirklich etwas abspielen.

## Stand

Wiedergabe ueber PipeWire -> `libspa-droid` -> Android-HAL funktioniert
(hoerbar bestaetigt). **Aufnahme und Telefonie noch nicht** - `callaudiod` ist
gegen libpulse gelinkt und erwartet die Ports der droid-card, im Profil
`pw-hal` ist Telefonie daher kaputt.

## Bauen

Die Upstream-Quellen werden nicht mitversioniert:

    mkdir -p src && git clone https://github.com/FuriLabs/pulseaudio-modules-droid-modern \
        src/pulseaudio-modules-droid-modern     # getestet mit d0e2330

    meson setup poc/spa-droid/build poc/spa-droid
    ninja -C poc/spa-droid/build
    ./install-hal.sh        # braucht sudo, aendert das aktive Profil NICHT

`poc/spa-droid/tools/port-droid-util.py` schneidet reproduzierbar die
Funktionen aus `common/droid-util.c`, die PulseAudio-Graphobjekte
dereferenzieren. Nach einem Upstream-Update erneut ausfuehren; das Skript
meldet, wenn seine Muster nicht mehr passen.

## Aufbau des Plugins

`libspa-droid.so` exportiert zwei Factories:

- **`api.droid.device`** - liest die Android-`audio_policy_configuration.xml`
  und meldet die HAL-Topologie als SPA-Device (9 Knoten auf diesem Geraet).
- **`api.droid.pcm`** - der Sink. `process()` schreibt in einen Ringpuffer,
  ein eigener `writer_thread` ruft den HAL, weil `pa_droid_stream_write`
  blockiert und deshalb nicht in den Graph-Thread darf.

## Fallen, die Zeit gekostet haben

- **Ein Hardware-Sink muss den Graphen takten.** Ohne `spa_node_call_ready()`
  aus einem eigenen timerfd ruft PipeWire `process()` nie auf - der Sink
  erscheint, bleibt aber stumm.
- **`spa_node_info.props` darf nicht NULL sein.** `module-adapter` reicht das
  ungeprueft an `pw_properties_update` weiter und segfaultet.
- **Ports muessen aus `hw->enabled_module` stammen.** `pa_droid_hw_module_get`
  dupliziert die Konfiguration, `pa_droid_open_output_stream` vergleicht per
  Zeigeridentitaet - Ports aus der eigenen Kopie werden immer abgelehnt.
- **Nach dem Oeffnen fehlen sonst Routing und Pegel**: ohne
  `pa_droid_stream_set_route()` und `set_volume(1.0)` bleibt der Stream stumm.
- **Node-Tests ohne Adapter sind unvollstaendig** - `probe-droid-node` spricht
  den Node direkt an und uebergeht genau die Schicht, die abgestuerzt ist.
- **`spa_log_info` ist unter PipeWires Standard-Loglevel unsichtbar.**
  Diagnose einschalten mit:

      systemctl --user set-environment SPA_DROID_DIAG=1

## Testen ohne Installation

    ninja -C poc/spa-droid/build
    # build/spa-merged/: Symlinks auf alle System-SPA-Plugins plus unser droid/
    SPA_PLUGIN_DIR=.../spa-merged pipewire -c poc/spa-droid/test-crash.conf
    PIPEWIRE_REMOTE=pipewire-droidtest pw-cli ls Node

`spa-inspect` taugt dafuer **nicht**: es dlopen't sein Argument direkt und
fragt `SPA_PLUGIN_DIR` gar nicht.
