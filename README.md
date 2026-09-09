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

Wiedergabe und Aufnahme ueber PipeWire -> `libspa-droid` -> Android-HAL
funktionieren, ebenso das Umschalten zwischen Lautsprecher, Ohrmuschel und
Headset. `callaudiod` erkennt die Karte und steuert sie (Sink, Source und
Ports). **Telefonie ist damit noch nicht fertig**: es fehlen ein
`voicecall`-Profil, `AUDIO_MODE_IN_CALL` (`pa_droid_hw_set_mode`) und der
Sprachpfad selbst.

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

## Aufbau

`libspa-droid.so` exportiert drei Factories:

- **`api.droid.device`** - die Karte. Liest die Android-Datei
  `audio_policy_configuration.xml`, meldet Profile und Routen und erzeugt die
  beiden Knoten. Die Routen heissen genau wie bei PulseAudios droid-card
  (`output-earpiece`, `output-speaker`, `input-builtin_mic`), weil callaudiod
  nach diesen Namen sucht.
- **`api.droid.pcm`** - der Sink. `process()` schreibt in einen Ringpuffer,
  ein eigener `writer_thread` ruft den HAL, weil `pa_droid_stream_write`
  blockiert und deshalb nicht in den Graph-Thread darf.
- **`api.droid.pcm.source`** - die Aufnahme, dasselbe rueckwaerts: ein
  `reader_thread` ruft das blockierende `pa_droid_stream_read`.

`wireplumber/droid.lua` bindet das Device ein. Der Umweg ueber WirePlumber ist
noetig: laesst man PipeWire das Device direkt aus `context.objects`
instanziieren, entstehen rohe SPA-Knoten ohne Adapter - die haben keine
Formatwandlung und erscheinen in pipewire-pulse gar nicht erst als Sink.

### Der Weg einer Routenaenderung

    pactl set-sink-port droid-sink output-earpiece
      -> Device (in WirePlumber): set_param(Route), merkt sich die Route
      -> SERIAL-Bit kippt -> Ereignis device-params-changed
      -> droid.lua reicht den Routennamen als SPA_PROP_params an den Knoten
      -> Knoten (im PipeWire-Daemon): pa_droid_stream_set_route()

Der Umweg ist noetig, weil Device und Knoten in **verschiedenen Prozessen**
laufen - nur der Knoten haelt den HAL-Stream.

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
- **Param-Aenderungen meldet man ueber das SERIAL-Bit**, nicht ueber das Feld
  `user` daneben: `params[i].flags ^= SPA_PARAM_INFO_SERIAL`. Ohne das erfaehrt
  niemand von der neuen Route - PipeWire liefert Clients weiter den alten
  zwischengespeicherten Wert, und WirePlumbers Routenpolitik ueberschreibt die
  Auswahl gleich wieder.
- **Puffergroesse ist das Graph-Quantum, nicht die HAL-Periode.** Beim Ausgang
  sind beide zufaellig 4096 B, beim Eingang liefert der HAL 3840 B - ein zu
  kleiner Puffer laesst libspa-audioconvert abstuerzen.
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
