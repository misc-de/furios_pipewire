# What we found out

The README says what this does and how to run it. This file says *why* it is
built the way it is: every measurement, every wrong turn, and the reasoning
behind decisions that look arbitrary in the code.

It exists because on this device almost nothing behaved the way the
documentation of the parts involved said it would, and each of those surprises
cost between an hour and three days. Where there is a number in here, it was
measured on the phone - none of this is reasoning from documentation, because
reasoning from documentation is what produced the wrong answers.

The headings are phrased as symptoms, so you can find your way in by what you
are seeing.

---

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


---

## Why telephony is fragile: callaudiod

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

---

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


---

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


---

## Bluetooth


**A2DP (music) works. Hands-free (HFP) works too, since 2026-09-11** - and the
reason it did not for so long was ours, twice over:

- **The headset sat in its charging case for every single test.** Earbuds
  accept the SCO link in there and drop it again after 25-50 ms; `btmon` says
  `Disconnect Complete, Reason: Remote User Terminated Connection (0x13)`. Out
  of the case the same link stands for as long as you want. Three "failed"
  calls and an entire wrong theory came out of that one detail. Ask about the
  case before measuring anything.
- **Bluetooth is commented out of this device's audio policy.** In
  `/android/vendor/etc/audio_policy_configuration.xml` lines 167-217 are a
  single XML comment, and it swallows every BT SCO and A2DP device port. The
  parsed configuration has nine device ports and no Bluetooth, so looking a
  port up by name found nothing and `hal_open()` fell through to the default
  output - **the speaker, without a word** - while every log line said
  `BT SCO`.

**SCO does not travel over HCI on this chip.** Measured with a link that really
stood: 3987 SCO packets sent, **zero received**, nothing audible at the headset,
microphone reading exactly zero. BlueZ is not where this audio is.

Where it is: MediaTek moves SCO between the Bluetooth chip and the
**application processor** over its own link, and the kernel exposes it as ALSA
device 55 - `BTCVSD`, playback and capture, 87 driver symbols in
`/proc/kallsyms`. The Android HAL opens that device and runs the codec in
software; `libcvsd_mtk.so` and `libmsbc_mtk.so` are both on the phone, and the
HAL's own strings name exactly the mixer controls the kernel driver offers
(`BTCVSD Band`, `BTCVSD Loopback Switch`, `BTCVSD Rx Timestamp`). Nothing about
that path is closed to us.

`51-bluez-ofono.conf` therefore sets the **native** backend - not for audio
through the host, which does not happen here, but because only that backend
offers a hands-free profile at all, and the air link has to exist before the
chip has anything to carry. Without an active stream on `bluez_output.*` in a
`headset-head-unit` profile there is no SCO connection. **PipeWire holds the
link, the HAL carries the sound.**

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
audio in either direction**. For a day that read as the end of the road. It was
not: the headset was in its case, so the link was gone 30 ms after it came up,
and the route to `BT SCO` was quietly landing on the speaker.

### What actually carries a Bluetooth call here

Three fixes in `droid-pcm.c`, every one of them found by measuring rather than
by reasoning:

1. **Resolve the route by its route name, not by the HAL port name.** The
   Bluetooth ports are missing from the audio policy, so
   `dm_config_find_port(module, "BT SCO")` returns NULL and the old code fell
   through to `dm_config_default_output_device()`. `apply_route()` now also
   remembers the route name, `hal_open()` resolves it through
   `port_by_route_name()`, and a route that cannot be honoured **warns**
   instead of silently playing somewhere else.
2. **Register the port we build ourselves.**
   `pa_droid_open_output_stream()` checks the port it is handed against
   `dm_config_find_device_port()`, which searches the module's own lists - so a
   port only we knew about was refused with
   `output stream "primary output" -> "BT SCO" failed`. It now goes into
   `module->ports` and `module->device_ports`, allocated to match the
   configuration's ownership rule: `ports` frees them, `device_ports` is a
   view.
3. **`BT_SCO=on` has to reach the HAL before the stream is opened.**
   `pa_droid_stream_set_route()` sends it as a side effect, but it needs an
   open stream - so on the first open after a route change nothing had told the
   HAL, and it opened a Bluetooth stream that stayed silent: the right PCM
   device, the chip even fetching the data, and nothing in the ear.
   `bt_sco_announce()` sends it first now, the way Android does.
4. **A route that crosses Bluetooth reopens the stream.** The HAL picks the
   hardware path while *opening*, not when a route is set on an open stream.
   Measured both ways on the phone: the route set on a running stream left the
   HAL on `pcmC0D0p` and the sound stayed in the speaker while every log line
   said "BT SCO"; set before opening, the HAL went to `pcmC0D55p`, the
   Bluetooth PCM device, and the chip fetched the data. `apply_route()` closes
   and reopens for that one move - and only that one. Speaker, earpiece and
   the wired accessories share the primary PCM device and reroute perfectly
   well on an open stream, which is how a call switches to the earpiece; a
   reopen there would buy nothing and cost a gap in the audio. If the new
   route cannot be opened, the node goes back to the one that was working
   rather than being left without a stream.
5. **Only one road actually reaches a node, and it is `droid.lua`.** The card
   runs inside WirePlumber's process and the nodes inside PipeWire's. A
   function call between them - `droid_node_set_route()` - therefore looks up
   an empty registry and returns `-ENOENT` every time, whatever name it is
   given. And the props the card publishes on its routes do not travel either:
   measured with the card publishing `droid.bt-wbs` and `droid.route` in one
   struct, the node received `droid.route` and nothing else. That one arrives
   because `droid.lua` reads the active route and sends the name on itself
   (`setNodeProp`). **So anything that has to reach a node goes through
   `droid.lua`.** The direct call was also passing the HAL's port name
   ("Speaker") where a route name ("output-speaker") was expected, which is
   fixed too - but the process boundary is the reason that path never worked,
   and "the route property only reaches the node while it runs" was never the
   whole story.

**Both sides have to agree on the codec, and nothing makes them.** The HAL
encodes narrow-band CVSD unless it is told otherwise, while WirePlumber
prefers the wide-band `headset-head-unit` profile (mSBC) - and then the
earbuds decode CVSD bytes as mSBC and nothing intelligible arrives. It is
invisible from the phone's side: the link stands, `BTCVSD Tx Irq` is on, the
HAL holds `pcmC0D55p`, every measurement says the audio is on its way.
Measured with the same tone each time:

| air link | HAL | in the ear |
|---|---|---|
| `headset-head-unit` (mSBC, 16 kHz) | default, `BTCVSD Band` = NB | **nothing** |
| `headset-head-unit-cvsd` (CVSD, 8 kHz) | default, `BTCVSD Band` = NB | **heard** |
| `headset-head-unit` (mSBC, 16 kHz) | `bt_wbs=on`, `BTCVSD Band` = WB | **heard** |

So the HAL does wide-band, it just has to be told: `bt_wbs=on` flips
`BTCVSD Band` from NB to WB and `Speech_BT_SCO_WB` to on, both observable in
the mixer. `probe-bt-sco-out --wbs` sends it by hand.

**Who tells the plugin:** `droid-bluetooth-call.lua` picks the headset's
profile, so it is the one place that knows the codec - `headset-head-unit` is
mSBC, `headset-head-unit-cvsd` is CVSD - and it sends `droid.bt-wbs` straight
to both nodes before it sets the routes. Both, because `bt_wbs` belongs to the
HAL module rather than to one stream and either node may open the next one;
before the routes, because setting a route is what makes the HAL open a stream
and the HAL reads the parameter while opening. A profile it does not recognise
tells the nodes nothing at all: the HAL then keeps its narrow-band default and
says so in the log, which beats a guess that is a coin toss between working
audio and silence.

Verified on the phone, end to end: codec announced, `bt_wbs=on` and
`BT_SCO=on` sent before the open, HAL on `pcmC0D55p`, `BTCVSD Band` on WB -
and the tone heard in the earbuds.

This is also why the measurement below once worked and later did not: the
profile happened to be CVSD that night.

The result, with the controls that make it an answer rather than an impression:

| | headset connected | headset disconnected |
|---|---|---|
| tone to `AUDIO_DEVICE_OUT_BLUETOOTH_SCO` | heard **in the earbud, phone silent**; `BTCVSD Tx Irq` **on** | `Tx Irq` **off**, nothing |
| capture from `AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET`, speaking | RMS **238**, peaks **+-6158**, `Rx Irq` **on**, HAL holds `pcmC0D55c` | RMS **2.3**, peaks **+-8**, `Rx Irq` **off** - and that while shouting |

A factor of 750 in peak between connected and disconnected: the audio comes
through the link, not from the phone's own microphone. The HAL takes both
8000 Hz mono and 48000 Hz stereo on that device; both were heard.

**Do not use scratching as a test sound.** The headset's noise suppression
removes it completely - RMS 2.3 while the built-in microphone measured 5713 on
the same gesture. It looks exactly like a dead microphone and it is not. Test
with a voice. And do not let a measuring tool judge on "exactly zero" either: a
live but quiet link decodes to a few counts of noise, and `probe-bt-sco-in`
happily called that "real audio" until it was given an RMS threshold.


### Still open, on the Bluetooth side

- **A real call has not been through this yet.** Whether the headset opens its
  microphone more readily once `AT+CLCC` reports an actual call is untested.
  PipeWire answers that question out of ModemManager, but only when
  `bluez5.hfphsp-backend-native-modem` is set - `modemmanager.c` returns early
  with "No modem allowed" otherwise, and then every `AT+CLCC` is answered "no
  calls" while the backend has just told the headset a call is active.

**The automatic hand-over is still off by default.**

    wpctl settings -s furios.bluetooth-call-routing true

turns it on, `false` turns it off again. It stays off until a real call has
been through the fixed path - a call without audio is worse than a call on the
earpiece, and this setting has produced exactly that before. The setting is
declared in `51-bluez-ofono.conf`; WirePlumber silently ignores a setting it
has no schema entry for, which is why the declaration is there and why
`tests/test-wireplumber-conf.sh` checks every name in those files.


### The explanation this file used to give, and why it was wrong

It said a Bluetooth call is a hardware path - BT chip -> PCM/I2S -> audio DSP ->
modem - with two ends to configure; that Android programs the controller end
through `BT_VND_OP_SCO_CFG` in `/android/vendor/lib64/libbt-vendor.so`; and that
BlueZ has no way to do the same. Every load-bearing part of that is wrong here:

- `libbt-vendor.so` is a **stub**: 676 bytes of code in `.text`. The operation
  it names does nothing on this device.
- The path is not hardware-only. SCO data reaches the application processor
  through ALSA device 55, and the HAL does the codec in software.
- `bluebinder` forwards SCO in **both** directions - packet type 0x03 to
  transaction 4 (`sendScoData`), transaction 4 back to packet type 0x03 into
  `/dev/vhci`. The bridge was never the bottleneck.
- The measurement the whole conclusion rested on - "the SCO channel delivers
  48000 samples with one distinct value" - came from `bluez_input.<address>`, a
  48 kHz loopback stub, at a time when no SCO link had lived longer than 45 ms.
  There was never a link to measure.

Worth keeping as a reminder: each of those statements had evidence behind it,
and the evidence was of the wrong thing. `hciconfig` showing `sco:0` is true and
means nothing about whether a headset can hear you.

**The host-side headset microphone delivers nothing**, and that part survives -
for a different reason than it used to give. SCO does not cross HCI on this
chip, so BlueZ's side of it is silent. The microphone works through the HAL.
VoIP over a headset therefore has to go the same way a mobile call does, through
BTCVSD, not through a socket in the host.

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
alike. Both of those readings were taken with the headset in its charging case,
so they say less than they looked like they did - but the setting stays off for
a reason that survived: what that node offers is the **host** side of SCO, and
SCO does not cross HCI on this chip. A microphone that records nothing is worse
than no microphone in the list, because picking it looks like it worked. The
setting is off in `51-bluez-ofono.conf`; the loopback source now
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


---

## Who gets to be the hands-free profile, and why it changes between boots

A voice memo was supposed to be recorded through the earbuds. The recorder
took the phone's own microphone instead, and the reason turned out to sit two
layers below the recorder.

The card had no hands-free profile at all:

    a2dp-sink-sbc, a2dp-sink-sbc_xq, a2dp-sink, off

No `headset-head-unit`, so `pactl set-card-profile` answered **No such
entity** - and without that profile there is no SCO channel, and without SCO
there is no microphone the HAL can reach. The log said why:

    spa.bluez5: ofono running, but not configured as HFP/HSP backend
    spa.bluez5.native: RegisterProfile() failed: org.bluez.Error.NotPermitted

BlueZ hands the `hfp_ag` UUID to whoever registers for it first, and exactly
one process can hold it. On this phone two want it: ofono, which is up early
because it is the modem stack, and WirePlumber's native backend. **Both lose
sometimes** - from one day's journal:

    13:52:00  ofonod     RegisterProfile() replied an error: UUID already registered
    14:17:11  wireplumber  spa.bluez5.native: RegisterProfile() failed: NotPermitted

So whether a Bluetooth headset can carry a call is decided by the order two
services happen to start in. The day the call was heard end to end,
WirePlumber had won. The day the memo failed, ofono had. Nothing in between
changed, and nothing announces which way it went except a line in the journal
and a profile that is quietly missing from the card.

Stopping ofono, restarting WirePlumber so it registers into the gap, and
starting ofono again brings the profiles back for that session:

    headset-head-unit-cvsd: Headset Head Unit (HSP/HFP, codec CVSD)
    headset-head-unit:      Headset Head Unit (HSP/HFP, codec MSBC)

**Restarting ofono leaves the modem switched off.** It comes back `Powered`
but `Online: false`, so the phone is registered nowhere and no call can reach
it - and nothing on screen necessarily says so. It has to be put back by hand:

    dbus-send --system --print-reply --dest=org.ofono /ril_0 \
        org.ofono.Modem.SetProperty string:Online variant:boolean:true

then `org.ofono.NetworkRegistration` reads `registered` again. Found the way
these things are usually found: by checking the state afterwards rather than
assuming the restart was free. Anyone reaching for this workaround has to
check the modem after it, every time.

That is a workaround for one session and not a fix - after the next boot it is
a coin toss again. **Still open:** either hand HFP to ofono for good
(`bluez5.hfphsp-backend = "ofono"`; ofono already offers the headset as
`/card_2`, type `gateway`, through `org.ofono.HandsfreeAudioManager`) or take
the Bluetooth hands-free plugin away from ofono so the native backend always
wins. One attempt at the first was made and it did nothing at all: the
fragment in `~/.config/wireplumber/wireplumber.conf.d/` never took effect - the
same "not configured as HFP/HSP backend" line came back after the restart -
so the configuration was not even read the way it was expected to be. That is
where this stands.

A WirePlumber restart with a headset connected also loses the whole Bluetooth
card, not just some profiles: the device stays connected in `bluetoothctl`
while `pactl list cards` no longer has it. Disconnect and connect brings it
back. Worth knowing before restarting anything with earbuds in.


## Recording from the headset outside a call

`audioctl bt-mic on` does what `droid-bluetooth-call.lua` does for a call,
because a voice memo needs exactly the same three things and is not a call:
the hands-free profile, the codec announced to the **nodes** before the route
is set, and `input-bluetooth_sco_headset` on the phone card. The HAL then
answers:

    set_parameters(bt_wbs=on)
    set_parameters(BT_SCO=on)
    Set mix port "primary input" input to AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET

Measured with a soundcore Liberty 4 Pro, 16 kHz mono, speaking into the
headset for 30 s: **18031 distinct values, RMS 1673, peak 27323**, with the
pauses between sentences reading RMS 1.0 and the speech RMS 3000-4400. The
phone's own microphone in the same room reads RMS 49 flat, so the two are not
easily confused once the level is looked at over time.

**And played back and heard: intelligible speech.** That part is not a
formality. Levels alone cannot tell a correct codec from a wrong one - a HAL
encoding CVSD into an mSBC link produces noise with perfectly healthy numbers,
which is exactly how a day went missing on the call path. Announcing
`bt_wbs=on` with the wideband profile is therefore confirmed by ear here, not
only by measurement.

### Two readings that looked like answers and were not

**The hold seemed unnecessary.** `51-bluez-ofono.conf` says the SCO link
stands while a stream is active on `bluez_output.*`, so a silent stream holds
it open while recording. A test with the hold stopped still came back at 940
distinct values, which read as "the profile alone carries it" - and the hold
was taken out of the code for about ten minutes. It was measured inside the
link the hold had just put up. Coming at it cold - back to `a2dp-sink`, then
the whole sequence with no hold - the same recording is **46394 samples, 1
distinct value, RMS 0**. The hold is needed. A measurement taken in the warmth
of the thing it is meant to rule out proves nothing.

**A silent recording looked like a broken headset.** After a run that reported
every step correctly, a 12 s recording came back at 1 distinct value while
somebody was speaking into the earbuds. `pcm55c` was `RUNNING` the whole time
- the HAL was reading the Bluetooth line, and the line was empty. The hold had
died: `paplay` was a child of the `audioctl` that started it and went down
with the command. `setsid` fixes it, and `bt-mic test` now says so out loud
when no hold is running, because that failure is indistinguishable from a dead
microphone at the level of the numbers.

### The one reading that settles it

`/proc/asound/card0/pcm55c/sub0/status` - the BTCVSD capture device. `RUNNING`
means the bytes are coming off the Bluetooth link; `closed` means they are
coming from somewhere else, whatever the route says and whatever the HAL
answered. It has to be read **while** the recording runs: checked afterwards
it is always `closed` again. `audioctl bt-mic status` shows it.


## Only one road reaches a node

The card runs inside WirePlumber's process, the nodes inside PipeWire's. Two
consequences, and both were learned the hard way:

- **A function call between them finds nothing.** `droid_node_set_route()`
  searches a registry that is empty in the other process and returns `-ENOENT`
  every single time. It never worked. It was also being handed the HAL's port
  name ("Speaker") where a route name ("output-speaker") was expected, which
  hid the real reason behind a plausible one.
- **The props the card publishes on its routes do not travel either.**
  Measured with the card publishing `droid.bt-wbs` and `droid.route` in one
  struct: the node received `droid.route` and nothing else.

That one arrives because **`droid.lua` reads the active route and sends the
name on itself** (`setNodeProp`). So anything that has to reach a node goes
through `droid.lua`. This file used to credit PipeWire with a hand-off it does
not make.

A related belief that was simply wrong: "the route property only reaches the
node while it runs". Measured - it arrives at an idle node too.

---

## Bluetooth calls need the codec, and nothing says so

The tone reached the Bluetooth PCM device and did not reach the ear. Every
counter said it was on its way: the link stood, `BTCVSD Tx Irq` was on, the
HAL held `pcmC0D55p`. The earbuds were decoding CVSD bytes as mSBC.

The HAL encodes narrow-band CVSD unless it is told otherwise, while WirePlumber
prefers the wide-band profile. Same tone each time:

| air link | HAL | in the ear |
|---|---|---|
| `headset-head-unit` (mSBC, 16 kHz) | default, `BTCVSD Band` = NB | **nothing** |
| `headset-head-unit-cvsd` (CVSD, 8 kHz) | default, `BTCVSD Band` = NB | **heard** |
| `headset-head-unit` (mSBC, 16 kHz) | `bt_wbs=on`, `BTCVSD Band` = WB | **heard** |

So the HAL does wide-band, it just has to be told. `bt_wbs=on` flips
`BTCVSD Band` from NB to WB and `Speech_BT_SCO_WB` to on, both visible in the
mixer. `probe-bt-sco-out --wbs` sends it by hand.

`droid-bluetooth-call.lua` picks the headset's profile, so it is the one place
that knows which codec was agreed, and it sends `droid.bt-wbs` to **both**
nodes **before** it sets the routes. Both, because `bt_wbs` belongs to the HAL
module rather than to one stream and either node may open the next one; before
the routes, because setting a route is what makes the HAL open a stream and the
HAL reads the parameter while opening. A profile it does not recognise tells
the nodes nothing at all - the HAL then keeps its default and the node says so
in the log, which beats a guess that is a coin toss between working audio and
silence.

**This is also why a measurement can be right one night and wrong the next.**
The night it worked, the profile happened to be CVSD. So: when measuring
anything over Bluetooth, write down the profile of the bluez card.

---

## A route change across Bluetooth has to reopen the stream

The HAL picks the hardware path while *opening* a stream, not when a route is
set on an open one. Measured both ways:

| | HAL holds | sound |
|---|---|---|
| route set while a stream ran | `pcmC0D0p` | the speaker, while every log line said "BT SCO" |
| route set before opening | `pcmC0D55p` | the headset |

`apply_route()` closes and reopens for that one move - and only that one.
Speaker, earpiece and the wired accessories share the primary PCM device and
reroute perfectly well on an open stream, which is how a call switches to the
earpiece; a reopen there would buy nothing and cost a gap in the audio. If the
new route cannot be opened, the node goes back to the one that was working
rather than being left without a stream.

---

## One look at a disconnect is not enough

Earbuds ran out of battery at 06:31:30 with a podcast playing.
`furios-audio-pause-on-disconnect` looked once, reported "nothing was playing",
and the podcast moved to the loudspeaker and played there for 66 minutes. It
was never a near miss: **0 successful pauses against 8 "nothing was playing"**
over the life of the service.

Reproduced with the service stopped, playing over Bluetooth and then
disconnecting: the player stays `Playing` the whole time and simply follows the
stream onto the speaker. So the code was right about what to do and wrong about
when to ask. Two ways the single look misses:

- **It is a race.** The disconnect signal, the player noticing its sink is
  gone, and PipeWire moving the stream all happen within a fraction of a
  second, in no fixed order. Across runs on the same phone the player reported
  `Playing` at t=0 sometimes and `Paused` other times.
- **A player too slow to answer looked exactly like a silent one.**
  `except GLib.Error: continue` said nothing at all, and on a phone that has
  just woken up two seconds is not a generous timeout.

So a disconnect is not a moment to inspect but a few seconds to watch:
`RETRY_DELAYS_MS`, seven more looks over twelve seconds, called off as soon as
something connects again - somebody putting their earbuds back in must not have
their music paused a few seconds later.

Proven on the device, the morning reconstructed:

    bluetooth gone, nothing was playing yet - still watching
    bluetooth gone, paused Emilia.instance229424 (on a second look)

---

## What Bluetooth audio actually costs

Measured during playback, CPU of `bluebinder` plus the codec:

| | |
|---|---|
| A2DP, AAC | **7.3 %** of a core |
| A2DP, SBC-XQ | 7.0 % |
| A2DP, SBC | **4.3 %** |
| the phone's own speaker, same file | 4.6 % |

AAC encoding alone costs 4.0 % against SBC's 1.0 %, so **SBC saves around 40 %**
and SBC-XQ saves nothing worth having. Bluetooth playback costs roughly 70 %
more than the same music through the speaker, and that surcharge goes to
`bluebinder`, the binder bridge to the Android BT HAL - every HCI packet
crosses it, 323 context switches a second while music plays. At idle it is
0.00 %: it costs only while Bluetooth audio runs.

**Resampling costs nothing**, which settles an older suspicion: 44.1 kHz
material over A2DP against 48 kHz material, 7.42 % versus 7.42 %. The note
about `clock.allowed-rates` allowing only 48000 is not a power question.

**And the stack itself is not a power problem.** At idle, against the shipped
one, over 60 s:

| | pw-hal | standard (PulseAudio) |
|---|---|---|
| CPU, pipewire/wireplumber | 0.00 % | 0.00 % |
| `ttyC0` (modem) holding the system awake | 45.2 % | 43.9 % |
| `bt_drv_io` / `bt_psm` | **nothing** | 2.5 % / 2.4 % |

No PCM is held open (`/proc/asound/card0/pcm0p/sub0/status` reads `closed`) and
every sink is suspended. PulseAudio keeps the Android HAL loaded exactly as we
do - 24 HAL mappings and 17 threads against our 12 and 15 - so
`hw_module_keepalive` is not a detour of ours, it is what the shipped stack
does too.

What does cost: **a phone that never sleeps**. `/sys/power/suspend_stats/success`
read **0** after 12 hours of uptime. And an application that holds an active
stream while playing silence keeps the sink `RUNNING` and the audio hardware
awake - worth checking before blaming the stack.

---

## What a security pass turned up

Five findings in our own code, one of them standing open on a running phone.

**The state file the whole device could write.** `postinst` set
`/var/lib/furios-audio` to 1777 and `profile` inside it to 0666, with a comment
explaining that audioctl writes it as the user. It does - but the answer to
that is owning the directory, not opening it to everyone. That file is read at
every login by `furios-audio-apply.service`, which then runs `audioctl set`
with whatever it says, so any account on the device could choose what the audio
stack does.

The directory now belongs to whoever installs (`SUDO_USER`/`PKEXEC_UID`),
failing that whoever already owns it, failing that whoever is logged in -
deliberately **not** "the first account above uid 1000", which on this phone is
the system account `radio` at 1001 while the person using it is `furios` at
32011. With nobody to name it stays root's and narrow, and audioctl says it
cannot persist a profile.

**Two insecure temporary files, both feeding sudo.** `build-bluez5-aac.sh`
built in `/tmp/pipewire-$VERSION-src`, reused that tree if it was already there
("sources already in ..."), and installed the resulting `.so` into `/usr` with
sudo - anyone could have put the tree there first. `install-hal.sh` wrote
`/tmp/pipewire-hal.conf` and installed it with sudo from a fixed name. Both use
the user's own cache or `mktemp` now, and a tree handed in through `SRC` is
refused unless it is theirs, not a symlink, and not writable by others.

**Two smaller ones.** `AUDIOCTL_PRIV` replaces the entire path to root and is a
test seam, so it is ignored once audioctl runs as root; and the switcher
resolved `audioctl` through `$PATH` before asking polkit for root on its
behalf, so the installed paths are tried first now.

**One finding was wrong**, and saying so is the point of this file: profile
names looked unchecked, and `preflight()` has refused them all along against
the one list in `$PROFILES`. A second list beside it is how two lists drift
apart, so the code came back out and only the test stayed.

What held up: no `eval`, `shell=True` or `os.system`; no unsafe C string
functions; every `memcpy` length bounded by `sizeof` or `MAX_CHANNELS`; the
root helper takes no paths from its caller and checks unit names against a
whitelist.

---

## Getting rid of root entirely

`audioctl` needed root for three things: masking units, writing a systemd
drop-in, and turning the droid monitor off. All three had a home-directory
equivalent the whole time, and systemd and WirePlumber read those **first**:

| needed root | does not |
|---|---|
| masks in `/etc/systemd/user/` | `~/.config/systemd/user/` |
| drop-in in `/etc/systemd/user/pipewire.service.d/` | `~/.config/systemd/user/pipewire.service.d/` |
| renaming `50-droid.conf` to `.off` in `/usr/share/` | a `99-` file in `~/.config/wireplumber/wireplumber.conf.d/` setting the components to `disabled` |

The third one is smaller than it looks: the monitor only has to be off in the
`pw-tunnel` profile. In `standard` WirePlumber is masked anyway, so the file
that loads it changes nothing there - which the file itself says.

**What that removed:** `furios-audio-helper` (112 lines), the polkit policy,
the authentication agent in the switcher app (about 180 lines, plus the polkit
bindings as a dependency), both `sudo` fallbacks, and the reason this project
had to care that FuriOS ships `furios ALL=(ALL) NOPASSWD:ALL`. The helper was
good work - a whitelist of unit names, no paths from the caller - but the best
version of a privileged component is the one that does not exist.

**What it added:** one migration. Masks left in `/etc/systemd/user` keep
masking, and audioctl can no longer remove them, so it warns with the command
that can: `sudo audioctl migrate`. That is now the only thing in audioctl that
wants root, run once.

**And it fixed the safety net on the way.** `furios-audio-apply.service` used
to skip the case where the stored profile is `standard`:

    test "$p" = standard || "$a" set "$p" || "$a" revert

A test profile leaves its masks and drop-in behind in the configuration, so
`audioctl try pw-hal` survived the reboot it was supposed to be discarded by -
which is the one thing `try` promises. Applying the stored profile
unconditionally is what discards it.

---

## The boot that never finished, and a phone with no sound

Applying the stored profile unconditionally was right, and it made a deadlock
that had been sitting there all along fire at **every** boot instead of only
when the stored profile was not `standard`.

`furios-audio-apply.service` runs `Before=pulseaudio.service pipewire.service`,
because a test profile must never outlive a reboot and the masks therefore have
to be in place before the stack reads them. It ran `audioctl set <profile>`,
and a switch restarts exactly those units:

    ExecStart=... "$a" set "$p" || "$a" revert     # inside: systemctl --user restart pipewire.service

systemd queues that restart behind the ordering it was given: pipewire may not
start until apply.service has finished. apply.service is waiting for the
restart. Neither moves again.

**What that looks like on the device** is not "the safety net did not work".
It is the whole session stuck: `systemctl --user list-jobs` showing
`pipewire.service start waiting` next to `furios-audio-apply.service start
running`, `default.target` never reached, no sound at all - and the deadlock
outlives the boot, so every switch attempted afterwards queues behind the same
job. The one found here had got as far as masking PulseAudio before it stopped,
which is why the phone had neither the new stack nor the old one.

**The fix is that there is nothing to start there.** The stack has not been
started yet; it starts from the configuration this unit writes, moments later.
So `audioctl boot` writes configuration and nothing else - `uctl()` drops
`start`, `stop`, `restart` and `reset-failed` while `BOOT=1`, and strips the
`--now` from `enable` (enabling is configuration, starting it is not).

Two things fall away with them, and both should:

- **`restart_audio_clients`** - callaudiod and feedbackd are not running yet.
- **`verify()`** - there is no sink to find, and fifteen seconds later it would
  have "fallen back to standard" over a stack that had simply not been started.

That last point needed a second unit. **The boot run cannot check whether the
profile produces sound** - it never could, the check ran before anything was
up - so `furios-audio-verify.service` runs `After=` the stack and asks there:

    ExecStart=... audioctl boot-check     # After= pipewire, pulseaudio, wireplumber

Ordering only, nothing wanted or required: which of those units runs is what
the profile decides, and a masked one must not keep the check waiting. It
allows the stack thirty seconds rather than the fifteen an interactive switch
gives it - a boot is slower than a switch on a warm system, and a fallback
that fires because the HAL was merely slow is worse than what it guards
against.

When there is no sink it applies `standard` and drops the test marker, and it
**leaves the stored profile alone**. That is deliberate and matches the
interactive fallback: a silent boot is a reason to make sound work now, not to
quietly discard a choice somebody made. `audioctl status` shows the mismatch,
and the next boot tries the profile again. If `standard` is silent too, the
unit is left failing - that is the one state nobody can hear their way out of,
and `systemctl --user --failed` is where it belongs.

**Both units now carry a `TimeoutStartSec`,** and that is the part worth
copying. The deadlock did not last because systemd had no opinion about it; it
lasted because nothing anywhere was going to end it. A unit that waits on the
audio stack needs a number that says how long, whatever else is right about it.

The tests read the calls audioctl makes to systemctl and fail if any of those
four verbs appears in a boot run, or if `--now` survives it - with a
counter-check that a switch from the command line still restarts things, so a
suppression that caught the interactive path too would not pass quietly.

---

## The sink monitor reads silence, whatever is playing

`droid-sink.monitor` returns digital silence while audio is demonstrably
playing - the HAL holding `pcmC0D0p`, the stream not corked, the tone audible.
Checked against a `module-null-sink` in the same session and the same way:
RMS 1243 there, RMS 0 and a single distinct sample value on ours.

**This invalidates a measurement made earlier in this repository.** When
looking into why a podcast played all morning, "the player holds an active
stream and sends digital silence" was concluded from exactly this monitor. The
same reading would have come back from a stream at full volume. What the hung
player actually sends is therefore unknown, and the morning is not explained
by silence.

Whatever a monitor port needs from a SPA node, this one is not providing it.
Nothing else in the stack depends on it - it is a measurement tool, not a
signal path - but it is a tool that answers the same thing to every question.

---

## How the tests are built, and why

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

## A second pass over the whole of our own code

Read end to end on 2026-09-12, looking for what breaks rather than for what is
missing: the SPA plugin, `audioctl`, the two installers, the packaging and the
Bluetooth watcher. Nine things came out of it. None of them was visible from
the outside, which is why they were still there.

**A failed `init` left the timer behind.** SPA does not call `clear()` on a
handle whose `init` returned an error - the loader frees the memory and is
done. `droid-pcm.c` created its timerfd and registered it with the data loop
early, and then had three plain `return`s below that: unreadable HAL
configuration, no "primary" module, no memory for the ring. Each one leaked the
fd and, worse, left the data loop polling a `spa_source` that points into
memory about to be freed. It needs a broken `audio_policy` XML to reach, which
is exactly the day nobody wants a second failure on top. There is one error
path now, and it undoes what was done.

**Capture overrun threw the ring away instead of the excess.** When nothing
picks the recorded audio up, the reader thread drops the oldest data. The new
read index was computed as `write + filled + r - RING_SIZE`, but `filled` is
already the distance between the two indices - adding it a second time pushed
the read index *past* the write index. Measured in the test: 28 kB left of a
256 kB ring, and in the window before the write index caught up, `process()`
saw a negative fill, padded the buffer with silence and counted an underrun for
audio that was there. It is `write + r - RING_SIZE`, and the test now insists
the ring stays full.

**Two ways the graph could have handed us memory we cannot use.**
`port_use_buffers` took any buffer, including one with no block or with
`datas[0].data` still NULL, and `process()` copies into that pointer on the
data thread - a crash in the middle of playback, a long way from its cause.
`port_set_io` took the io area without checking its size, where `impl_set_io`
next to it had always checked. Both are refused now, at negotiation time, which
is where a wrong answer is still an error message.

**A clock that cannot be rearmed said nothing.** `on_timeout()` ignored the
result of arming the next tick. If that ever fails the node stops producing and
looks perfectly healthy doing it - the same class of silence the timer checks
at startup were added for. It says so once now.

**What comes out of a state file is not a profile until it is checked.**
`current_profile()` passed the contents of `profile`/`profile.try` straight on.
A truncated, half-written or older-format file therefore reached `preflight()`,
which exits on a name it does not know - and the command that reads it is
`audioctl boot`, the one that runs at every single boot and whose only job is
to leave the phone able to make a sound. It falls back to `standard` and says
so, `apply()` refuses anything that is not a profile, and `audioctl restart`
now runs the preconditions it used to skip (restarting into `pw-hal` with no
plugin present was a silent phone with a cheerful message).

**The boot check could have been killed in the middle of its own rescue.**
`furios-audio-verify.service` allowed 90 s. Its worst case is 30 s waiting for
a sink, then applying `standard` - stopping, masking and restarting units -
then waiting again. On a slow boot systemd could have sent SIGTERM halfway
through that switch, with PulseAudio already masked and PipeWire not yet up:
the exact silent phone the unit exists to prevent, caused by the unit. The
budget is the whole fallback plus room now (240 s), and it is only ever spent
when there is no sound anyway - the unit runs after the stack and holds nothing
that makes noise.

**`install.sh` threw away a profile somebody had set.** It wrote `standard`
into the state file unconditionally, so re-running the installer - after a
`git pull`, say - quietly moved the phone back to the shipped stack at the next
boot. The package's `postinst` had always written it only when there was
nothing there; the script does the same now. Its state directory handling was
wrong in the other direction as well: `mkdir -p` succeeds on a directory that
exists and belongs to root, so the fallback that fixes the ownership never ran
and the write below it took the whole install down.

**`chown -R` in `postinst`.** The directory belongs to an unprivileged user who
can put anything in it, a hard link to a file elsewhere included, and the
recursive chown that an upgrade runs as root would hand them that file. Only
the directory and the two state files we write there are chowned now, symlinks
skipped. Hardlink protection in the kernel makes this hard to exploit today;
that is a reason not to rely on it.

**Any Bluetooth device disconnecting paused the music.** The watcher reacted to
`Connected = false` on `org.bluez.Device1` without asking what the device was,
so a watch, a keyboard or a car's data link going out of range stopped a
podcast - and on a phone those drop off far more often than earbuds run flat.
It asks BlueZ for the device's UUIDs now and only acts on the audio profiles
(A2DP, headset, hands-free). Where the answer cannot be had - the device is
already gone from the bus, BlueZ does not answer, the property is not what it
should be - it counts as audio and pauses. Doubt resolves towards pausing,
because that costs a press of play and the other way costs a morning.

## A hands-free profile is not a hands-free link

A Bluetooth call had no audio in either direction, while everything that can
be inspected said it should. This is what was actually wrong, and what was
merely in the way.

**In the way: ofono and WirePlumber both want BlueZ' hands-free registration.**
ofono's built-in `hfp_ag_bluez5` plugin and WirePlumber's native bluez5 backend
both register the `hfp_ag` UUID, exactly one gets it, and the loser says so
once:

    spa.bluez5.native: RegisterProfile() failed: org.bluez.Error.NotPermitted

When WirePlumber loses, the Bluetooth card carries A2DP profiles only. No
`headset-head-unit`, so no SCO, so neither the headset microphone nor a call -
and nothing says so anywhere a user would look. It is not specific to this
stack: the shipped PulseAudio loses the same race (2026-09-12, 15:13:38, same
error). ofono starts as a system service long before the user session, so it
wins almost every boot; the two sessions in the journal where it lost were
both restarts into a running session.

The fix is to decide it up front instead of by startup order, and to decide it
in ofono, because the other direction is already ruled out:
`bluez5.hfphsp-backend = "ofono"` cannot be used here - only the native backend
offers a hands-free profile on this device, which is why `51-bluez-ofono.conf`
sets `native` with a page of reasons. So ofono gives the plugin up:

    ExecStart=/usr/sbin/ofonod --nodetach -P hfp_ag_bluez5

After that `/bluetooth/profile/hfp_ag` is gone from ofono while `hfp_hf` and
`dun_gw` stay, WirePlumber registers without complaint, and the card offers
`headset-head-unit` (mSBC) and `headset-head-unit-cvsd`, both available. Modem
telephony does not go through that plugin and was unaffected - the modem stayed
`Online: true` across the restart, which is worth checking every time, because
it has come back `false` before.

**The actual problem: nobody holds the link.** With the registration settled
and `furios.bluetooth-call-routing` on, the take-over during a real call was
flawless. Two seconds after the call arrived:

    15:26:11 call=voicecall01 out=output-earpiece       in=input-builtin_mic
    15:26:11 FIX out war output-earpiece                 <- callaudiod, once
    15:26:14 call=voicecall01 out=output-bluetooth_sco  in=input-bluetooth_sco_headset
             ... unchanged until the call ended ...

Profile `headset-head-unit`, both routes set, `bt_wbs` announced, callaudiod
pushing the ports away exactly once at the start - `MAX_DEFENDS = 3` was never
close to being used up. And the call was silent in both directions.

What is missing is the SCO link itself. A hands-free profile means the card
*can* carry one; it does not make one exist. On this chip the link exists only
while a stream is active on `bluez_output.*` - and in a call nobody opens one,
because the voice path runs modem <-> DSP and never reaches the host. There is
no stream to be had, so the profile sits there with nothing underneath it.

Repeating the same call with `audioctl bt-mic on` holding a stream of zeroes on
`bluez_output` underneath it, the output routed to `output-bluetooth_sco`, and a
thin watcher putting the ports back when callaudiod took them: **heard, both
directions.**

So the routing in `droid-bluetooth-call.lua` is right and incomplete, and so is
`audioctl bt-call`, whose `bt_hold` defends the *port* and not the *link*. The
setting stays off until something holds a link for the length of a call.

**And a measuring instrument fell over.** `/proc/asound/card0/pcm55p` and
`pcm55c` stayed `closed` through the call that was working and being heard.
They show host streams - a recording, a playback - not telephony. Reading them
during a call and concluding anything from them is reading the wrong dial: for
the microphone outside a call they are still the only reading that settles
where the samples came from, but for a call the ear is the instrument.
