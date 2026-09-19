-- SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
-- SPDX-License-Identifier: MIT
-- Putting a call on a headset without anyone typing anything.
local T = dofile((os.getenv("TEST_ROOT") or ".") .. "/tests/lua/harness.lua")
local wp = T.wp

T.suite("bluetooth call")

local function droid_card(profile, routes)
  return wp.object({ ["device.api"] = "droid-hal" }, { params = {
    Profile = { { name = profile } },
    Route = routes or {
      { name = "output-earpiece", device = 0 },
      { name = "input-builtin_mic", device = 1 },
    },
    EnumRoute = {
      { name = "output-earpiece", index = 0, devices = { 0 } },
      { name = "input-builtin_mic", index = 1, devices = { 1 } },
      { name = "output-bluetooth_sco", index = 2, devices = { 0 } },
      { name = "input-bluetooth_sco_headset", index = 3, devices = { 1 } },
    },
  } })
end

-- A headset as PipeWire publishes one. The hands-free profile is called
-- headset-head-unit whatever codec it ends up on - the codec is in the
-- description, built as "Headset Head Unit (HSP/HFP, codec %s)" - so a test
-- that wants a narrow-band device says so here and not in the name.
local function bt_card(codec)
  return wp.object({ ["device.api"] = "bluez5", ["bound-id"] = 7 }, { params = {
    EnumProfile = {
      { name = "a2dp-sink", index = 0,
        description = "High Fidelity Playback (A2DP Sink, codec SBC)" },
      { name = "headset-head-unit", index = 1,
        description = "Headset Head Unit (HSP/HFP, codec " ..
                      (codec or "MSBC") .. ")" },
    },
  } })
end

-- The headset's own nodes. api.bluez5.codec on them is the codec BlueZ really
-- negotiated, which is the only reading that can tell one headset from
-- another - and the one that arrives late, because the codec is agreed when
-- the SCO link goes up.
local function bt_nodes(card, codec)
  local out = {}
  for _, name in ipairs { "bluez_output.9C_DF_03_BB_8D_31.1",
                          "bluez_input.9C_DF_03_BB_8D_31.0" } do
    table.insert(out, wp.add("node", wp.object({
      ["device.id"] = tostring(card["bound-id"]),
      ["node.name"] = name,
      ["api.bluez5.codec"] = codec,
    })))
  end
  return out
end

-- The automatic routing is off unless someone turns it on; every test that
-- wants to see it work has to say so, exactly like the phone does.
local function setup(enabled)
  wp.install()
  wp.settings["furios.bluetooth-call-routing"] = enabled ~= false
  T.load_script(T.root .. "/wireplumber/droid-bluetooth-call.lua")
end

local function fire(dev)
  local hook = wp.hooks["monitor/droid-bluetooth-call"]
  T.traced(function () T.traced(function () hook.execute({ get_subject = function () return dev end }) end) end)
  -- The takeover waits for callaudiod to finish; in a test the main loop is
  -- us, so run whatever is waiting.
  T.traced(function () wp.fire_timers() end)
end

local function route_names_set()
  local names = {}
  for _, c in ipairs(wp.calls_of("set_params")) do
    local body = c.args[3] and c.args[3].body
    -- Only what carries an index: profiles and routes. The codec travels as
    -- Props and has none, and counting it here once made this look like a
    -- third route.
    if body and body.index ~= nil then table.insert(names, tostring(body.index)) end
  end
  return names
end

-- What the script told the phone's NODES about the Bluetooth codec. It goes to
-- them directly, the way droid.lua sends the route: the card lives in
-- WirePlumber's process and the nodes in PipeWire's, and nothing carries a
-- card parameter across that by itself.
local function codecs_told()
  local out = {}
  for _, c in ipairs(wp.calls_of("set_param")) do
    local body = c.args[3] and c.args[3].body
    if c.args[2] == "Props" and body and body.params and body.params.body then
      local kv = body.params.body
      if kv[1] == "droid.bt-wbs" then table.insert(out, kv[2]) end
    end
  end
  return out
end

-- The phone's two nodes. The codec goes to them directly - the card cannot
-- carry it, it lives in another process - so they have to be there to be told.
local function droid_nodes(dev)
  for _, d in ipairs { 0, 1 } do
    wp.add("node", wp.object({
      ["device.id"] = tostring(dev["bound-id"]),
      ["card.profile.device"] = tostring(d),
      ["node.name"] = d == 0 and "droid-sink" or "droid-source",
    }))
  end
end

-- A call starts with a headset connected: profile, both routes.
setup()
local dev = wp.add("device", droid_card("voicecall"))
wp.add("device", bt_card())
droid_nodes(dev)
fire(dev)
T.check("the call sets something on both cards", #wp.calls_of("set_params") >= 3)
T.check_equal("the phone card is routed twice - output and input", 2,
              #route_names_set() - 1)

-- The codec has to be told, and told before the routes: setting a route is
-- what makes the HAL open a stream, and the HAL reads the codec while
-- opening. Told too late, the headset decodes CVSD bytes as mSBC and plays
-- nothing at all - while the link, the interrupt counters and the PCM device
-- all say the audio is on its way.
local told = codecs_told()
T.check_equal("both nodes are told the codec", 2, #told)
T.check_equal("and a wide-band link is reported as wide-band", "on", told[1])
T.check_equal("for the input side too", "on", told[2])
-- Order across the two objects: every codec call has to come before the first
-- route, because the route is what makes the HAL open a stream and the HAL
-- reads the codec while opening.
local last_codec, first_route = 0, nil
for i, c in ipairs(wp.calls) do
  if c.what == "set_param" and c.args[2] == "Props" then
    local body = c.args[3] and c.args[3].body
    if body and body.params and body.params.body
       and body.params.body[1] == "droid.bt-wbs" then
      last_codec = i
    end
  elseif c.what == "set_params" and c.args[2] == "Route" then
    first_route = first_route or i
  end
end
T.check("the codec is told before the first route",
        first_route ~= nil and last_codec > 0 and last_codec < first_route)

-- The car. Hands-free unit, HFP 1.5, SDP SupportedFeatures 0x0007 - no
-- wide-band bit, so the link can only be CVSD, and PipeWire says so in the
-- profile description. Read off the profile NAME instead, this announced
-- wide-band and the call of 2026-09-18 23:02 was silent in both directions
-- with everything else about it correct.
setup()
local car = wp.add("device", droid_card("voicecall"))
wp.add("device", bt_card("CVSD"))
droid_nodes(car)
fire(car)
local car_told = codecs_told()
T.check_equal("a narrow-band car is told as narrow-band", "off", car_told[1])
T.check_equal("for the input side too", "off", car_told[2])

-- The link beats the profile: the description says what the profile stands
-- for, the node says what BlueZ agreed. When they disagree the agreement wins.
setup()
local both = wp.add("device", droid_card("voicecall"))
local both_card = wp.add("device", bt_card("CVSD"))
bt_nodes(both_card, "msbc")
droid_nodes(both)
fire(both)
T.check_equal("what the link runs beats what the profile is called", "on",
              codecs_told()[1])

-- Some builds hang the codec on the card rather than on its nodes.
setup()
local oncard = wp.add("device", droid_card("voicecall"))
local oncard_card = bt_card("CVSD")
oncard_card.properties["api.bluez5.codec"] = "msbc"
wp.add("device", oncard_card)
droid_nodes(oncard)
fire(oncard)
T.check_equal("the codec is found on the card too", "on", codecs_told()[1])

-- The codec arrives late. While the profile is still switching, the nodes
-- carry the A2DP codec and the plain profile has no description to read - so
-- nothing is announced yet and the HAL keeps its narrow-band default. Once
-- the link is up the next round says so, and the plugin reopens the HAL
-- stream by itself.
setup()
local late = wp.add("device", droid_card("voicecall"))
local late_card = wp.add("device", wp.object(
  { ["device.api"] = "bluez5", ["bound-id"] = 7 },
  { params = { EnumProfile = { { name = "headset-head-unit", index = 1 } } } }))
local late_nodes = bt_nodes(late_card, "sbc")
droid_nodes(late)
fire(late)
T.check_equal("a codec nobody can name yet is not guessed", 0, #codecs_told())
wp.reset()
for _, n in ipairs(late_nodes) do n.properties["api.bluez5.codec"] = "msbc" end
wp.fire_timers()
T.check_equal("and is announced as soon as the link says it", "on",
              codecs_told()[1])
T.check_equal("to both nodes", 2, #codecs_told())

-- Told once, not again and again: the HAL reopens its stream on every change,
-- so repeating the same answer would cost a gap in the call for nothing.
wp.reset()
wp.fire_timers()
T.check_equal("an unchanged codec is not announced again", 0, #codecs_told())

-- A codec bt_wbs has no word for - LC3-SWB is neither on nor off - is not
-- rounded to the nearest one. The HAL keeps its default and the log says so.
setup()
local swb = wp.add("device", droid_card("voicecall"))
local swb_card = wp.add("device", bt_card("LC3-SWB"))
bt_nodes(swb_card, "lc3_swb")
droid_nodes(swb)
fire(swb)
T.check_equal("a codec bt_wbs cannot express is not guessed at", 0,
              #codecs_told())

-- The call ends while the watch is armed: the next round has to stand down
-- rather than talk to a call that is over.
setup()
local over = wp.add("device", droid_card("voicecall"))
local over_card = wp.add("device", bt_card())
bt_nodes(over_card, "cvsd")
droid_nodes(over)
fire(over)
over.params.Profile = { { name = "default" } }
fire(over)
wp.reset()
wp.fire_timers()
T.check_equal("the codec watch stops when the call does", 0, #codecs_told())

-- A headset whose hands-free profiles we cannot name: no codec is invented for
-- it. The HAL then keeps its narrow-band default and says so in the log, which
-- beats guessing - the wrong guess is silence that measures perfectly.
setup()
local dev2 = wp.add("device", droid_card("voicecall"))
wp.add("device", wp.object({ ["device.api"] = "bluez5" }, { params = {
  EnumProfile = { { name = "a2dp-sink", index = 0 } },
} }))
droid_nodes(dev2)
fire(dev2)
T.check_equal("a headset with no hands-free profile is told no codec", 0,
              #codecs_told())

-- The same event again while the call runs, with the card now reporting the
-- Bluetooth route: there is nothing to defend, so nothing is set.
wp.reset()
dev.params.Route = {
  { name = "output-bluetooth_sco", device = 0 },
  { name = "input-bluetooth_sco_headset", device = 1 },
}
fire(dev)
T.check_equal("a route still on Bluetooth is left alone", 0,
              #wp.calls_of("set_params"))

-- callaudiod pulls the port back to the earpiece: it must be set again.
wp.reset()
dev.params.Route = { { name = "output-earpiece", device = 0 } }
fire(dev)
T.check("a route moved away is set again", #wp.calls_of("set_params") >= 1)

-- The call ends.
wp.reset()
dev.params.Profile = { { name = "default" } }
dev.params.Route = { { name = "output-bluetooth_sco", device = 0 } }
fire(dev)
T.check("hanging up puts things back", #wp.calls_of("set_params") >= 1)

-- A call with no headset: the phone keeps it, and nothing is touched.
setup()
dev = wp.add("device", droid_card("voicecall"))
fire(dev)
T.check_equal("a call without a headset changes nothing", 0,
              #wp.calls_of("set_params"))

-- An event about some other card is not ours to act on.
setup()
wp.add("device", bt_card())
fire(wp.object({ ["device.api"] = "alsa" }))
T.check_equal("an event about another card is ignored", 0,
              #wp.calls_of("set_params"))

-- The headset offers only the narrowband profile.
setup()
dev = wp.add("device", droid_card("voicecall"))
wp.add("device", wp.object({ ["device.api"] = "bluez5", ["bound-id"] = 7 }, { params = {
  EnumProfile = { { name = "headset-head-unit-cvsd", index = 4 } } } }))
droid_nodes(dev)
fire(dev)
T.check("the narrowband profile is accepted when it is all there is",
        #wp.calls_of("set_params") >= 2)
T.check_equal("and its name is read as narrow-band", "off",
              codecs_told()[1] or "nothing told")

-- A card with no Bluetooth SCO route: the attempt is abandoned rather than
-- left half applied.
setup()
dev = wp.add("device", droid_card("voicecall", nil))
dev.params.EnumRoute = { { name = "output-earpiece", index = 0, devices = { 0 } } }
wp.add("device", bt_card())
fire(dev)
wp.reset()
dev.params.Profile = { { name = "default" } }
fire(dev)
T.check_equal("a half-applied call is not undone on hang-up, because it never began",
              0, #wp.calls_of("set_params"))

-- The headset disappears mid-call, then the call ends: putting its profile
-- back has nothing to put it back on.
setup()
dev = wp.add("device", droid_card("voicecall"))
local card = wp.add("device", bt_card())
fire(dev)
wp.objects.device = { dev }        -- the headset is gone
wp.reset()
dev.params.Profile = { { name = "default" } }
fire(dev)
T.check("hanging up without the headset still restores the phone routes",
        #wp.calls_of("set_params") >= 1)

-- Something inside throws. The hook has to give up quietly rather than take
-- the monitor - and every sound with it - down.
setup()
local exploding = wp.object({ ["device.api"] = "droid-hal" })
exploding.iterate_params = function () error("no params today") end
fire(exploding)
local warned = false
for _, c in ipairs(wp.calls_of("log")) do
  if tostring(c.args[2]):find("giving up", 1, true) then warned = true end
end
T.check("an error inside is caught and reported, not thrown", warned)

-- Off is off: a call with a headset connected is left entirely alone. This is
-- the state the phone ships in, and it is the state a real call needs until
-- the automatic routing has been shown to work in one.
setup(false)
dev = wp.add("device", droid_card("voicecall"))
wp.add("device", bt_card())
fire(dev)
T.check_equal("with the setting off a call is not touched at all", 0,
              #wp.calls_of("set_params"))

-- A setting nobody declared reads as nothing, and nothing has to mean off.
setup(false)
wp.settings["furios.bluetooth-call-routing"] = nil
dev = wp.add("device", droid_card("voicecall"))
wp.add("device", bt_card())
fire(dev)
T.check_equal("a setting that does not exist leaves the call alone too", 0,
              #wp.calls_of("set_params"))

-- The fight that cost a real call: callaudiod keeps pulling the port back.
-- After a few rounds the call has to go back to the phone rather than have
-- the voice path rebuilt two or three times a second.
setup()
dev = wp.add("device", droid_card("voicecall"))
wp.add("device", bt_card())
fire(dev)                                   -- the call starts on the headset
for _ = 1, 4 do
  dev.params.Route = { { name = "output-earpiece", device = 0 } }
  fire(dev)
end
local gave_up = false
for _, c in ipairs(wp.calls_of("log")) do
  if tostring(c.args[2]):find("back to the phone", 1, true) then gave_up = true end
end
T.check("a route that will not stay put makes it give up", gave_up)

wp.reset()
dev.params.Route = { { name = "output-earpiece", device = 0 } }
fire(dev)
T.check_equal("and then it stops trying for the rest of the call", 0,
              #wp.calls_of("set_params"))

-- Once that call is over the next one may try again.
wp.reset()
dev.params.Profile = { { name = "default" } }
fire(dev)
dev.params.Profile = { { name = "voicecall" } }
dev.params.Route = { { name = "output-earpiece", device = 0 } }
fire(dev)
T.check("the next call is not held against the last one",
        #wp.calls_of("set_params") >= 1)

-- The headset is connected in the middle of a running call. The phone card
-- says nothing then - its profile and routes have not moved - so the only
-- notice is the Bluetooth card announcing itself.
setup()
dev = wp.add("device", droid_card("voicecall"))
fire(dev)                                   -- a call, no headset yet
T.check_equal("a call that starts without a headset is left alone", 0,
              #wp.calls_of("set_params"))

wp.reset()
local late = wp.add("device", bt_card())
fire(late)                                  -- the headset announces itself
T.check("a headset connected mid-call is picked up", #wp.calls_of("set_params") >= 3)

-- The same event with no phone card anywhere is not something to act on.
setup()
wp.add("device", bt_card())
wp.objects.device = { wp.objects.device[1] }   -- only the headset exists
fire(wp.objects.device[1])
T.check_equal("a headset with no phone card changes nothing", 0,
              #wp.calls_of("set_params"))

-- And when there is no call, a headset coming and going is none of our
-- business.
setup()
dev = wp.add("device", droid_card("default"))
local idle = wp.add("device", bt_card())
fire(idle)
T.check_equal("a headset outside a call is left alone", 0,
              #wp.calls_of("set_params"))

-- The Bluetooth card is not touched while callaudiod is still setting the call
-- up. Pulling the cards around underneath it is what wedged it for 25 seconds.
setup()
dev = wp.add("device", droid_card("voicecall"))
wp.add("device", bt_card())
local hook = wp.hooks["monitor/droid-bluetooth-call"]
wp.reset()
T.traced(function () hook.execute({ get_subject = function () return dev end }) end)
T.check_equal("nothing is set while callaudiod is still working", 0,
              #wp.calls_of("set_params"))
T.check("but a takeover is scheduled", #wp.calls_of("timeout_add") == 1)
T.check("and not immediately", wp.calls_of("timeout_add")[1].args[1] >= 1000)
T.traced(function () wp.fire_timers() end)
T.check("afterwards it takes over", #wp.calls_of("set_params") >= 3)

-- The call can be over before the delay is up.
setup()
dev = wp.add("device", droid_card("voicecall"))
wp.add("device", bt_card())
hook = wp.hooks["monitor/droid-bluetooth-call"]
T.traced(function () hook.execute({ get_subject = function () return dev end }) end)
dev.params.Profile = { { name = "default" } }
T.traced(function () hook.execute({ get_subject = function () return dev end }) end)   -- hung up
wp.reset()
T.traced(function () wp.fire_timers() end)
T.check_equal("a call that ended before the delay is not taken over", 0,
              #wp.calls_of("set_params"))

-- And it can throw once it runs, long after the event that scheduled it. The
-- hook's own pcall is gone by then, so the timer needs its own.
setup()
dev = wp.add("device", droid_card("voicecall"))
wp.add("device", bt_card())
hook = wp.hooks["monitor/droid-bluetooth-call"]
T.traced(function () hook.execute({ get_subject = function () return dev end }) end)
dev.iterate_params = function () error("the card went away") end
wp.reset()
T.traced(function () wp.fire_timers() end)
local caught = false
for _, c in ipairs(wp.calls_of("log")) do
  if tostring(c.args[2]):find("giving up", 1, true) then caught = true end
end
T.check("a takeover that throws is caught, not thrown at the monitor", caught)

-- The wait is for quiet, not for a span of time. A real call showed why: the
-- profile goes to voicecall while the phone is still ringing, so a fixed delay
-- started there ran out in the middle of callaudiod's work - 14 ms before
-- callaudiod set the port it wanted.
setup()
dev = wp.add("device", droid_card("voicecall"))
wp.add("device", bt_card())
hook = wp.hooks["monitor/droid-bluetooth-call"]
T.traced(function () hook.execute({ get_subject = function () return dev end }) end)
T.check_equal("the call arms a wait", 1, #wp.calls_of("timeout_add"))

-- callaudiod is still moving things: another event before the timer fires.
T.traced(function () hook.execute({ get_subject = function () return dev end }) end)
T.check_equal("something moving arms it again", 2, #wp.calls_of("timeout_add"))

wp.reset()
T.traced(function () wp.fire_timers() end)
local profiles = 0
for _, c in ipairs(wp.calls_of("set_params")) do
  if c.args[2] == "Profile" then profiles = profiles + 1 end
end
T.check_equal("but the card is still taken over exactly once - the timer armed "
              .. "before the move stands down", 1, profiles)

T.done()
