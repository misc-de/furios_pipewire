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

Beim Wechsel beendet audioctl ausserdem `callaudiod` und `feedbackd`. Beide
halten dauerhafte Verbindungen zum Audioserver und ueberleben einen
Serverwechsel nicht: callaudiod findet danach keine Karte mehr, feedbackd
scheitert an "Invalid state" - der Klingelton bleibt aus und nur die
Vibration kommt noch. D-Bus startet beide bei Bedarf neu.

## Stand

Wiedergabe und Aufnahme ueber PipeWire -> `libspa-droid` -> Android-HAL
funktionieren, ebenso das Umschalten zwischen Lautsprecher, Ohrmuschel und
Headset. Die Steuerkette der Telefonie ist vollstaendig:

| callaudiod            | Karte              | HAL                       |
|-----------------------|--------------------|---------------------------|
| `SelectMode(1)`       | Profil `voicecall` | `AUDIO_MODE_IN_CALL`      |
| `EnableSpeaker(false)`| `output-earpiece`  | Route `Earpiece`          |
| `EnableSpeaker(true)` | `output-speaker`   | Route `Speaker`           |
| `SelectMode(0)`       | Profil `default`   | `AUDIO_MODE_NORMAL`       |

**Am Geraet bestaetigt (2026-09-09)**: echtes Telefonat im Profil `pw-hal` -
Klingelton hoerbar, beide Seiten hoeren einander, Lautsprecher-Taste schaltet
hoerbar um, Medienwiedergabe danach unveraendert. Der Journalauszug dazu:

    19:25:25  gnome-calls startet callaudiod
    19:25:29  Profil voicecall -> AUDIO_MODE_IN_CALL -> Route Earpiece am HAL
    19:25:41  Lautsprecher-Taste -> Route Speaker am HAL
    19:25:48  wieder Ohrmuschel  -> Route Earpiece am HAL
    19:25:50  Auflegen -> Profil default -> AUDIO_MODE_NORMAL

Damit callaudiod das tut, muss dreierlei stimmen, und jedes davon hat gekostet:

- Der Sink muss `device.api = "droid-hal"` melden (die Kennung von
  PulseAudios droid-Modul). Nur dann nimmt callaudiod seinen Droid-Pfad und
  schaltet zwischen `default` und `voicecall`; mit `droid` faellt es auf den
  ALSA-UCM-Weg zurueck, sucht Profile mit Praefix `HiFi`/`Voice Call` und tut
  nichts ("set_card_profile: nothing to be done").
- Die Profile muessen genau `default` und `voicecall` heissen.
- Kabelports muessen als **nicht verfuegbar** gemeldet werden. Bei
  Droid-Karten waehlt callaudiod sonst sofort das Headset - im Anruf landete
  der Ton statt auf der Ohrmuschel im Nirgendwo. PulseAudio meldet sie auf
  diesem Geraet ebenfalls als nicht verfuegbar; eine Klinkenerkennung gibt es
  hier nicht (in `/sys/class/extcon` steht nur USB).

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

## Bluetooth

Damit PipeWire Bluetooth-Audio kann, muss `libspa-0.2-bluetooth` installiert
sein - ohne das Paket gibt es gar keine BT-Unterstuetzung, WirePlumber meldet
nur "BlueZ SPA plugin is missing or broken".

Fuers Freisprechen (HFP) ist **`native` der richtige Backend**, nicht `ofono`
(siehe `wireplumber/51-bluez-ofono.conf`): ofono hat auf diesem Geraet kein
`org.ofono.Handsfree`-Interface und meldet unter `HandsfreeAudioManager` auch
bei verbundenem Kopfhoerer keine Karten. Mit dem ofono-Backend bietet die
Bluetooth-Karte deshalb ausschliesslich A2DP-Profile - telefonieren ueber den
Kopfhoerer ist unmoeglich, die Ein- und Ausgaenge dafuer existieren nicht
einmal.

**Nach einem Profilwechsel den Kopfhoerer einmal neu verbinden.** audioctl
startet WirePlumber neu; ein Geraet, das die Verbindung schon vorher hatte,
registriert seine Profile nicht vollstaendig neu - die Karte zeigt dann nur
einen Teil (etwa nur HFP, kein A2DP). Nach `bluetoothctl disconnect` und
`connect` sind beide da.

## Testen ohne Installation

    ninja -C poc/spa-droid/build
    # build/spa-merged/: Symlinks auf alle System-SPA-Plugins plus unser droid/
    SPA_PLUGIN_DIR=.../spa-merged pipewire -c poc/spa-droid/test-crash.conf
    PIPEWIRE_REMOTE=pipewire-droidtest pw-cli ls Node

`spa-inspect` taugt dafuer **nicht**: es dlopen't sein Argument direkt und
fragt `SPA_PLUGIN_DIR` gar nicht.
