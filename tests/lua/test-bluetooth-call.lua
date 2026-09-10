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

local function bt_card()
  return wp.object({ ["device.api"] = "bluez5" }, { params = {
    EnumProfile = {
      { name = "a2dp-sink", index = 0 },
      { name = "headset-head-unit", index = 1 },
    },
  } })
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
  T.traced(function () hook.execute({ get_subject = function () return dev end }) end)
end

local function route_names_set()
  local names = {}
  for _, c in ipairs(wp.calls_of("set_params")) do
    local body = c.args[3] and c.args[3].body
    if body then table.insert(names, tostring(body.index)) end
  end
  return names
end

-- A call starts with a headset connected: profile, both routes.
setup()
local dev = wp.add("device", droid_card("voicecall"))
wp.add("device", bt_card())
fire(dev)
T.check("the call sets something on both cards", #wp.calls_of("set_params") >= 3)
T.check_equal("the phone card is routed twice - output and input", 2,
              #route_names_set() - 1)

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
wp.add("device", wp.object({ ["device.api"] = "bluez5" }, { params = {
  EnumProfile = { { name = "headset-head-unit-cvsd", index = 4 } } } }))
fire(dev)
T.check("the narrowband profile is accepted when it is all there is",
        #wp.calls_of("set_params") >= 2)

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

T.done()
