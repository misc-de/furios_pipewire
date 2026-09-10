# furios_pipewire

Audio auf dem FuriPhone (FuriOS, MediaTek mt6877) laeuft ab Werk ueber
**PulseAudio + module-droid-card**; das mitgelieferte PipeWire ist auf Kamera
und Screencast beschraenkt (`pipewire-droid.conf` laedt kein `api.alsa.*`).

Dieses Verzeichnis (`~/Projekte/furios_pipewire`) enthaelt zweierlei:

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

Vor dem Wechsel nach `pw-hal` prueft audioctl, ob das Plugin noch zur
installierten PipeWire-Fassung passt (`gebaut-gegen` neben der Plugin-Datei).
Bricht ein Systemupdate die SPA-Schnittstelle, waere `pw-hal` sonst
kommentarlos stumm - jetzt kommt eine Warnung mit dem Hinweis, neu zu bauen.

`audioctl rescue` macht den Ton wieder hoerbar: Auslieferungszustand,
Lautsprecher statt Ohrmuschel, nicht stumm, 65 %. `bt-call` faengt Abbrueche
mit einer Trap ab, damit ein unterbrochener Test nichts Stummes hinterlaesst.

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

## Als Paket

    ./packaging/build-deb.sh
    sudo dpkg -i packaging/furios-audio-pipewire_*.deb

Das Paket bringt alles mit: Plugin, WirePlumber-Monitor und -Konfiguration,
audioctl, die Umschalter-App samt Symbol, die systemd-Units. Es installiert
nach `/usr`, die Skripte unten nach `/usr/local` - audioctl findet seine
Dateien in beiden Pfaden.

Die Abhaengigkeit auf `pipewire (>= X, << X+1)` ist Absicht: das Plugin wird
gegen eine bestimmte SPA-Schnittstelle gebaut. Bricht ein Update sie, waere der
Ton sonst kommentarlos weg - so haelt apt das Paket zurueck, und `audioctl`
warnt zusaetzlich vor dem Umschalten.

Entfernen mit `sudo dpkg -r furios-audio-pipewire`. Steht dabei `pw-hal`
persistent, warnt das Paket vorher - sonst waere nach dem naechsten Neustart
kein Ton mehr da.

## Bauen

Die Upstream-Quellen werden nicht mitversioniert:

    mkdir -p src && git clone https://github.com/FuriLabs/pulseaudio-modules-droid-modern \
        src/pulseaudio-modules-droid-modern     # getestet mit d0e2330

    meson setup poc/spa-droid/build poc/spa-droid
    ninja -C poc/spa-droid/build
    ./install-hal.sh        # braucht sudo, aendert das aktive Profil NICHT

Alternativ das Paket bauen (siehe oben) - das ist der Weg, der ein
Systemupdate uebersteht.

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

## Umschalten per Knopfdruck

    ./gui/install.sh

Installiert einen kleinen GTK4/libadwaita-Umschalter samt Symbol und
Startereintrag ("Audio Switch" im Anwendungsraster; die Oberflaeche ist
englisch). Ein Schalter fuer
den Stack, darunter was tatsaechlich laeuft, und ein Knopf **Ton
wiederherstellen** - der stellt den Auslieferungszustand her, schaltet auf den
Lautsprecher, hebt die Stummschaltung auf und setzt eine hoerbare Lautstaerke.
Genau die Kombination, die nach einem misslungenen Test wie "gar nichts geht
mehr" aussieht.

Standardmaessig merkt sich der Schalter die Auswahl **nicht**: ein Neustart
fuehrt zum Auslieferungszustand zurueck. Wer es anders will, legt vorher den
zweiten Schalter um.

Waehrend des Umschaltens laeuft ein **pulsierender Fortschrittsbalken**, in dem
steht, was audioctl gerade meldet - die Zeilen werden gelesen, waehrend das
Programm noch laeuft. Bewusst ohne Prozentzahl: wie lange es dauert, weiss
vorher niemand, denn audioctl wartet bis zu 15 Sekunden auf einen Sink. Eine
erfundene Zahl, die bei 90 % haengen bleibt, waere schlechter als gar keine.

## VoIP und Mikrofonwahl

Die Karte bietet vier Knoten: `droid-sink`/`droid-source` fuer alles Normale
und `droid-voip-sink`/`droid-voip-source` fuer den VoIP-Pfad des HAL
(mixPorts `voip_rx`/`voip_tx`). Letztere haben niedrige Prioritaet - dort
landet nichts versehentlich. Ihre Aufnahme benutzt die Android-Audioquelle
`voice communication`, fuer die der HAL seine Sprachaufbereitung einschaltet.

Zwei Eigenheiten, die dabei Blut gekostet haben:

- **Routen nur auf dem primaeren Strom.** `pa_droid_stream_set_route()` prueft
  das mit einer Zusicherung und **bricht den ganzen Prozess ab**, wenn man es
  auf einem anderen mixPort versucht. Der VoIP-Knoten riss PipeWire so
  reproduzierbar mit (SIGABRT). Das Routing gilt ohnehin fuer alle offenen
  Stroeme - der primaere gibt es vor.
- **16 kHz, nicht 48.** Der portierte Code erzwingt fuer `voip_rx` genau das
  ("Override voip_rx channel map (mono) and sample rate (16000)"). Der Knoten
  richtet sich deshalb nach `audio.rate` aus seinen Eigenschaften, statt stur
  48 kHz anzubieten - sonst schriebe er mit dreifacher Geschwindigkeit hinein.

`wireplumber/droid-input-follows-output.lua` laesst das Mikrofon der
Ausgabewahl folgen, wenn beide Enden zum selben Geraet gehoeren. Ohne das
bleibt das Mikrofon beim Telefon, sobald callaudiod Sink und Source einmal
festgenagelt hat.

## Echo im Gespraech

Die Gegenseite hoert sich selbst? Software hilft dagegen nicht: bei einem
Mobilfunkgespraech laeuft der Sprachweg **Modem <-> DSP**, nicht ueber den
Rechner - eine Echounterdrueckung in PipeWire oder PulseAudio saehe die Daten
nie. Die Unterdrueckung sitzt im DSP und heisst bei MediaTek **DMNR**
(Zweimikrofonverfahren gegen Stoergeraeusche und Echo).

Die Abstimmungsdatei des Herstellers sagt auf diesem Geraet:

    MTK_DUAL_MIC_SUPPORT         yes   zwei Mikrofone sind vorhanden
    MTK_HANDSFREE_DMNR_SUPPORT   yes   der Chip kann Freisprech-DMNR
    MTK_INCALL_HANDSFREE_DMNR    no    im Anruf ist sie abgeschaltet
    MTK_VOIP_HANDSFREE_DMNR      no
    MTK_VOIP_NORMAL_DMNR         no

Zum Ausprobieren gibt es einen Schalter - in der Umschalter-App unter "Call
echo", oder auf der Kommandozeile:

    furios-audio-dmnr an       # geaenderte Kopie einhaengen
    furios-audio-dmnr aus      # zurueck zum Original
    furios-audio-dmnr status

Er legt per Bind-Mount eine geaenderte Kopie ueber die Datei - die Partition
ist schreibgeschuetzt und per dm-verity abgesichert, daran wird nicht
geschraubt - und startet den Audiostack neu (`audioctl restart`, das Profil
bleibt), damit der HAL sie liest. Ein Neustart des Geraets raeumt alles weg.
**Ob es hilft, ist ungetestet** - das kann nur ein echtes Gespraech zeigen.

Zwei Spuren, die sich als Sackgasse erwiesen haben:

- `realcall=on`, das PulseAudios Kartenmodul beim Anrufprofil schickt, **lehnt
  dieser HAL ab** (`failed: -22`). Offenbar Qualcomm-Erbe. Der Code dafuer ist
  da, die Option aber aus.
- `speaker_before_voice=true` ist dagegen aktiv: der HAL routet jetzt vor dem
  Moduswechsel kurz auf den Lautsprecher, wie es upstream fuer Geraete
  empfiehlt, die den Anruf sonst falsch beginnen.

## Bluetooth

Damit PipeWire Bluetooth-Audio kann, muss `libspa-0.2-bluetooth` installiert
sein - ohne das Paket gibt es gar keine BT-Unterstuetzung, WirePlumber meldet
nur "BlueZ SPA plugin is missing or broken".

**A2DP (Musik) funktioniert. Freisprechen (HFP) nicht - und zwar auf keinem
Stack, auch nicht im Auslieferungszustand.** Nachgemessen:

- ofono besitzt HFP: `hfp_ag_bluez5` ist fest eingebaut, `/bluetooth/profile/hfp_ag`
  ist bei BlueZ registriert. PipeWires nativer Backend scheitert daneben an
  `listen(): Address already in use` und `RegisterProfile() failed: NotPermitted`.
- ofono legt fuer den verbundenen Kopfhoerer trotzdem keine Karte an
  (`HandsfreeAudioManager.GetCards` bleibt leer).
- Auf dem Auslieferungs-PulseAudio ist das Ergebnis dasselbe: `handsfree_head_unit`
  laesst sich waehlen, die Quelle geht auf `RUNNING` und liefert **0 Bytes**;
  `paplay` in die Gegenrichtung blockiert.
- Die HAL-Konfiguration kennt BT-SCO-Geraeteports, aber weder unsere Karte noch
  die von PulseAudio fuehren sie: auf Android-Geraeten laeuft SCO in Hardware
  zwischen BT-Chip und Audio-DSP, nicht ueber den Rechner.

Deshalb bleibt es beim **ofono-Backend**: mit `native` erscheinen HFP-Profile,
die stumm bleiben, und WirePlumber schaltet beim ersten Aufnahmeversuch dorthin
um - was auch die Musikwiedergabe verdirbt.

**`priority.session` muss am Knoten gesetzt sein.** Fehlt sie, faellt
WirePlumbers Geraetewahl auf `priority.driver` zurueck - und der ist hier
50000, damit der Sink den Graphen taktet. Damit schlaegt der Knoten sogar eine
ausdrueckliche Benutzerwahl, die mit 30000 gewichtet wird: es liesse sich
ueberhaupt kein anderes Ausgabegeraet mehr auswaehlen, kein
Bluetooth-Kopfhoerer, nichts. Das Symptom ist tueckisch, weil die Auswahl in
der Oberflaeche einfach wirkungslos bleibt, ohne Fehlermeldung.

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
