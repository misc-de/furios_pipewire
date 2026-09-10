-- SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
-- SPDX-License-Identifier: MIT
-- The monitor: it creates the nodes and carries route, mode and volume across
-- the process boundary to them.
local T = dofile((os.getenv("TEST_ROOT") or ".") .. "/tests/lua/harness.lua")
local wp = T.wp

T.suite("droid monitor")

local function card(profile, routes, enum)
  return wp.object({ ["device.api"] = "droid-hal", ["bound-id"] = "5" },
    { params = {
        Profile = { { name = profile or "default" } },
        Route = routes or {},
      } })
end

local function node(name, dev)
  return wp.object({
    ["node.name"] = name,
    ["device.id"] = "5",
    ["card.profile.device"] = tostring(dev),
    ["device.api"] = "droid-hal",
    ["media.class"] = dev == 0 and "Audio/Sink" or "Audio/Source",
  })
end

local function setup(with_mixer)
  wp.install()
  wp.mixer = with_mixer and wp.object({}) or nil
  if wp.mixer then wp.mixer.volume_answer = { volume = 0.5, mute = false } end
  T.load_script(T.root .. "/wireplumber/droid.lua")
end

local function props_sent()
  local out = {}
  for _, c in ipairs(wp.calls_of("set_param")) do
    local body = c.args[3] and c.args[3].body
    if body and body.params then
      table.insert(out, { key = body.params.body[1], value = body.params.body[2] })
    end
  end
  return out
end

local function fire(dev)
  local hook = wp.hooks["monitor/droid-forward-device-params"]
  T.traced(function () hook.execute({ get_subject = function () return dev end }) end)
end

-- The device is created at load, and the nodes follow from its objects.
setup(true)
T.check_equal("the card is created once", 1, #wp.calls_of("spa_device"))
T.check("and the mixer is looked for", #wp.calls_of("plugin_find") == 1)

-- A route change reaches the node that holds the HAL stream.
setup(true)
local dev = wp.add("device", card("default", { { name = "output-speaker", device = 0 } }))
wp.add("node", node("droid-sink", 0))
wp.reset()
fire(dev)
local sent = props_sent()
local by_key = {}
for _, s in ipairs(sent) do by_key[s.key] = s.value end
T.check("something is sent to the node", #sent >= 1)
T.check_equal("the route travels under the key the node listens for",
              "output-speaker", by_key["droid.route"])
-- The mode goes first on purpose: entering a call the HAL routes to the
-- earpiece by itself, and an explicit route has to win afterwards.
T.check_equal("and the mode goes first", "droid.mode", sent[1] and sent[1].key)

-- The same route again: the node is not told twice.
wp.reset()
fire(dev)
T.check_equal("the same route is not sent again", 0, #props_sent())

-- The call profile becomes a mode for the HAL, and only the playback node can
-- set it.
setup(true)
dev = wp.add("device", card("voicecall", { { name = "output-earpiece", device = 0 } }))
wp.add("node", node("droid-sink", 0))
wp.add("node", node("droid-source", 1))
wp.reset()
fire(dev)
local keys = {}
for _, s in ipairs(props_sent()) do keys[s.key] = s.value end
T.check_equal("the call mode is sent", "call", keys["droid.mode"])
T.check("and the voice volume with it", keys["droid.voice-volume"] ~= nil)

-- The capture node hears about the mode too - the HAL keeps its audio source
-- otherwise and the microphone reads silence after every call.
local to_source = 0
for _, c in ipairs(wp.calls_of("set_param")) do
  if c.args[1].properties["node.name"] == "droid-source" then to_source = to_source + 1 end
end
T.check("the capture node is told about the mode as well", to_source >= 1)

-- The communication profile is its own mode.
setup(true)
dev = wp.add("device", card("communication"))
wp.add("node", node("droid-sink", 0))
wp.reset()
fire(dev)
keys = {}
for _, s in ipairs(props_sent()) do keys[s.key] = s.value end
T.check_equal("the communication profile is its own mode", "communication",
              keys["droid.mode"])

-- Volume on the route is put back on the node, because the HAL cannot
-- attenuate and PipeWire stops doing it once a route reports a level.
setup(true)
dev = wp.add("device", card("default", {
  { name = "output-speaker", device = 0,
    props = { properties = { channelVolumes = { 0.25, 0.25 }, mute = false } } },
}))
wp.add("node", node("droid-sink", 0))
wp.reset()
fire(dev)
local applied = false
for _, c in ipairs(wp.calls_of("set_param")) do
  local body = c.args[3] and c.args[3].body
  if body and body.channelVolumes then applied = true end
end
T.check("a volume on the route is applied to the node", applied)

-- Without a mixer the script still runs; the call volume simply stays put.
setup(false)
dev = wp.add("device", card("voicecall"))
wp.add("node", node("droid-sink", 0))
wp.reset()
fire(dev)
keys = {}
for _, s in ipairs(props_sent()) do keys[s.key] = s.value end
T.check_equal("without a mixer the mode is still sent", "call", keys["droid.mode"])
T.check("and no voice volume is", keys["droid.voice-volume"] == nil)

-- An event about a card that is not ours.
setup(true)
wp.reset()
fire(wp.object({ ["device.api"] = "bluez5" }))
T.check_equal("an event about another card is ignored", 0, #wp.calls_of("set_param"))

-- --- the parts that only run through a callback ---------------------------

-- Creating a node: the monitor is handed an object by the card and has to turn
-- it into an adapter node with the properties the graph needs.
--
-- The card here is the one the script created, not one the test added - the
-- callback looks the device up rather than being handed it, and the script's
-- own device is what it finds.
setup(true)
dev = wp.objects.device[1]
dev["bound-id"] = "5"
dev.params = {
  Profile = { { name = "default" } },
  Route = { { name = "output-speaker", device = 0 } },
}
local create_object, object_removed
for _, c in ipairs(wp.calls_of("connect")) do
  if c.args[1] == "create-object" then create_object = c.args[2] end
  if c.args[1] == "object-removed" then object_removed = c.args[2] end
end
T.check("the monitor listens for objects from the card", create_object ~= nil)

wp.reset()
local parent = wp.object({ ["bound-id"] = "5" })
T.traced(function ()
  create_object(parent, 0, "node", "api.droid.pcm",
                { ["node.name"] = "droid-sink", ["card.profile.device"] = "0" })
end)
local made = wp.calls_of("node")
T.check_equal("a node is made", 1, #made)
T.check_equal("as an adapter, or it has no format conversion", "adapter",
              made[1] and made[1].args[1])
T.check_equal("carrying the factory the adapter must load", "api.droid.pcm",
              made[1] and made[1].args[2]["factory.name"])
T.check_equal("and the card it belongs to", "5",
              made[1] and made[1].args[2]["device.id"])

-- The node finishes activating: it is stored, and told the current route.
-- The card activates itself when the script loads, so the interesting one is
-- the activation that carries a callback: the node's.
local activate
for _, c in ipairs(wp.calls_of("activate")) do
  if type(c.args[2]) == "function" then activate = c end
end
wp.add("node", node("droid-sink", 0))
wp.reset()
T.traced(function () activate.args[2](nil, nil) end)
T.check_equal("an activated node is stored", 1, #wp.calls_of("store_managed_object"))
T.check("and is told the state it was born too late for",
        #wp.calls_of("set_param") >= 1)

-- Activation fails: it is reported and nothing is stored.
wp.reset()
T.traced(function () activate.args[2](nil, "no such factory") end)
T.check_equal("a node that failed to activate is not stored", 0,
              #wp.calls_of("store_managed_object"))

-- The card takes an object back: the monitor forgets it.
wp.reset()
T.traced(function () object_removed(parent, 0) end)
local forgotten = wp.calls_of("store_managed_object")
T.check_equal("an object taken back is forgotten", 1, #forgotten)
T.check("by storing nothing in its place", forgotten[1].args[2] == nil)

-- The mixer reports a volume change. Outside a call it is the graph's business;
-- during one it has to reach the HAL.
setup(true)
dev = wp.add("device", card("voicecall"))
local sink = wp.add("node", node("droid-sink", 0))
sink["bound-id"] = "11"
fire(dev)
local changed
for _, c in ipairs(wp.calls_of("connect")) do
  if c.args[1] == "changed" then changed = c.args[2] end
end
T.check("the monitor listens to the mixer", changed ~= nil)

wp.mixer.volume_answer = { volume = 0.9, mute = false }
wp.reset()
T.traced(function () changed(wp.mixer, "11") end)
local vol_sent = false
for _, s in ipairs(props_sent()) do
  if s.key == "droid.voice-volume" then vol_sent = true end
end
T.check("a volume change during a call reaches the HAL", vol_sent)

-- A mixer that answers nothing.
wp.mixer.volume_answer = nil
wp.reset()
T.traced(function () changed(wp.mixer, "11") end)
T.check_equal("and a mixer with no answer changes nothing", 0, #props_sent())

-- Outside a call the mixer is not our business.
setup(true)
dev = wp.add("device", card("default"))
sink = wp.add("node", node("droid-sink", 0))
sink["bound-id"] = "11"
fire(dev)
for _, c in ipairs(wp.calls_of("connect")) do
  if c.args[1] == "changed" then changed = c.args[2] end
end
wp.reset()
T.traced(function () changed(wp.mixer, "11") end)
T.check_equal("outside a call the mixer is left to the graph", 0, #props_sent())

-- A route whose props carry no volume at all, and one that repeats a level.
setup(true)
dev = wp.add("device", card("default", {
  { name = "output-speaker", device = 0, props = { properties = {} } },
}))
wp.add("node", node("droid-sink", 0))
wp.reset()
fire(dev)
local any_volume = false
for _, c in ipairs(wp.calls_of("set_param")) do
  local body = c.args[3] and c.args[3].body
  if body and body.channelVolumes then any_volume = true end
end
T.check("a route without a level sets none", not any_volume)

-- The card fails to appear at all: the script says so instead of carrying on.
wp.install()
wp.spa_device_fails = true
T.load_script(T.root .. "/wireplumber/droid.lua")
wp.spa_device_fails = false
local notified = false
for _, c in ipairs(wp.calls_of("log")) do
  if tostring(c.args[2]):find("not be loaded", 1, true) then notified = true end
end
T.check("a card that cannot be created is reported", notified)

-- The same level twice: the node is told once.
setup(true)
dev = wp.add("device", card("default", {
  { name = "output-speaker", device = 0,
    props = { properties = { channelVolumes = { 0.4, 0.4 }, mute = false } } },
}))
wp.add("node", node("droid-sink", 0))
fire(dev)
wp.reset()
fire(dev)
local repeated = false
for _, c in ipairs(wp.calls_of("set_param")) do
  local body = c.args[3] and c.args[3].body
  if body and body.channelVolumes then repeated = true end
end
T.check("the same level is not sent twice", not repeated)

-- Applying the level throws. It must be caught: an error here would take the
-- monitor down, and a volume is not worth a phone with no card.
setup(true)
dev = wp.add("device", card("default", {
  { name = "output-speaker", device = 0,
    props = { properties = { channelVolumes = { 0.2, 0.2 }, mute = true } } },
}))
local breaking = wp.add("node", node("droid-sink", 0))
-- Only the volume throws. The route travels the same way but outside the
-- pcall, and making that fail would just abort the test rather than exercise
-- the handler.
breaking.set_param = function (self, name, pod)
  if pod and pod.body and pod.body.channelVolumes then error("nope") end
  wp.record("set_param", self, name, pod)
end
wp.reset()
fire(dev)
local warned = false
for _, c in ipairs(wp.calls_of("log")) do
  if tostring(c.args[2]):find("not applied", 1, true) then warned = true end
end
T.check("a level that cannot be applied is reported, not thrown", warned)

T.done()
