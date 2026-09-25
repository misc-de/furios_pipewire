# bluez5: HFP AG answers AT+CLCC with call index 0 — strict car kits never see the call

**Repo:** pipewire/pipewire (spa/plugins/bluez5, native backend with ModemManager)
**Checked against:** `master` @ `55a19b4cd3eb771d4d3fc9e66eb4f3bef465dd0f` and tag `1.6.6`
**Observed on:** Debian PipeWire 1.6.6-1, WirePlumber 0.5.14, BlueZ 5.85-4, FuriPhone FLX1 (FuriOS), ModemManager API provided by ofono2mm; car: Audi A6 2013, MMI 3G (CSR controller, LMP 2.1, HFP 1.5)
**Workaround in this repo:** `tools/furios-audio-bluez5-fix.py` (patches the load of `call->index` in a runtime copy of libspa-bluez5.so)
**Patch:** [`pipewire-1-clcc-call-index.patch`](pipewire-1-clcc-call-index.patch) (applies to master, compiles with `-Wall -Wextra`)

## Summary

`struct call` has an `index` field, and `backend-native.c` sends it as the
first field of every `+CLCC` line. Nothing ever assigns it: `modemmanager.c`
creates the call with `calloc()` and leaves `index` at 0. So every call is
reported as

```
+CLCC: 0,0,3,0,0,"+49…",145
```

3GPP TS 27.007 §7.18 defines `<idx>` as the call identification number, an
integer starting at **1** (it is the same number `AT+CHLD=1x/2x` refer to).
Index 0 is not a valid call.

A hands-free unit that supports enhanced call status (HF feature bit 5) and
trusts `+CLCC` drops the entry. For the car there is no call: the display shows
nothing, the radio keeps playing, and although the SCO link is up and audio is
routed, the car neither plays nor records a word. Earbuds and most headsets
never send `AT+CLCC`, which is presumably why this has gone unnoticed.

## Observed

The car connects with `AT+BRSF=103` (0x67: EC/NR, three-way, CLI, enhanced
call status, enhanced call control) and polls `AT+CLCC` about once a second
for the whole call. HCI capture (btmon) of an outgoing call, unpatched:

```
19:16:08.366  TX  +CIEV: 3,3
19:16:08.503  RX  AT+CLCC
19:16:08.503  TX  +CLCC: 0,0,3,0,0,"+4915…",145
19:16:09.760  TX  +CIEV: 2,1
...                (SCO link up, CVSD)
19:16:17.350  TX  +CIEV: 2,1
19:16:17.424  TX  +CIEV: 3,0
19:16:17.473  TX  +CLCC: 0,0,0,0,0,"+4915…",145     <- active, index 0
```

The car's display stayed empty and the radio kept playing throughout; three
calls, all the same.

Same setup with only the index changed to 1 (binary patch of the load of
`call->index` in the `+CLCC` reply, everything else identical):

```
TX  +CLCC: 1,0,…          (105 replies during the call)
```

The car showed the call, muted the radio, and speech worked in both
directions.

## Cause

`spa/plugins/bluez5/modemmanager.c`, handler for `CallAdded`:

```c
call_object = calloc(1, sizeof(struct call));
...
call_object->this = this;
call_object->path = strdup(path);
spa_list_append(&this->call_list, &call_object->link);   /* index stays 0 */
```

`grep index spa/plugins/bluez5/modemmanager.c` finds nothing on master. The
only reader is `backend-native.c` (`AT+CLCC` handler):

```c
rfcomm_send_reply(rfcomm, "+CLCC: %u,%u,%u,0,%u,\"%s\",%d", call->index, ...);
```

## Fix

Give each call the lowest free index ≥ 1 when ModemManager announces it, the
way a modem numbers its calls. A freed number is reused, so a second call
while one is active gets 2, and after the first ends a new one gets 1 again.

```diff
+static unsigned int mm_next_call_index(struct impl *this)
+{
+	/* 3GPP TS 27.007 +CLCC: call identification numbers start at 1. Reuse the
+	 * lowest free one, like a modem does. */
+	unsigned int index = 1;
+	struct call *call;
+
+again:
+	spa_list_for_each(call, &this->call_list, link) {
+		if (call->index == index) {
+			index++;
+			goto again;
+		}
+	}
+	return index;
+}
...
+		call_object->index = mm_next_call_index(this);
 		spa_list_append(&this->call_list, &call_object->link);
```

`CallAdded` is the only place a `struct call` is created, and `+CLCC` is the
only reader of `index`, so nothing else needs to change.

Not verified: the patched source has not been built into a full PipeWire and
run in the car. It has been checked that it applies to master and compiles.
What was tested in the car is a binary patch that makes the reply send 1.

## Possibly related, not part of this report

While alerting, before the call is answered, the AG sends `+CIEV: 2,1`
(call = 1) together with the SCO setup, while `+CIND?` right after still
reports `call=0, callsetup=3`. The car tolerated it once the index was fixed,
so it is not the cause here, but the indicators disagree with each other.
