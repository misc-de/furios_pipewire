-- SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
-- SPDX-License-Identifier: MIT
-- Which sink the audio goes to by default: Bluetooth takes over when it
-- connects, and the VoIP path never does.
local T = dofile((os.getenv("TEST_ROOT") or ".") .. "/tests/lua/harness.lua")
local wp = T.wp

T.suite("default sink policy")

local function setup()
  wp.install()
  T.load_script(T.root .. "/wireplumber/droid-default-sink-policy.lua")
end

local function bt_connects(node)
  local hook = wp.hooks["node/droid-bluetooth-takes-over"]
  T.traced(function ()
    hook.execute({ get_subject = function () return node end })
  end)
end

local function default_sink_changed(name)
  local hook = wp.hooks["node/droid-voip-never-default"]
  T.traced(function ()
    hook.execute({ get_properties = function ()
      return { ["event.subject.value"] = '{"name":"' .. name .. '"}' }
    end })
  end)
end

local function droid_card(profile)
  return wp.object({ ["device.api"] = "droid-hal" },
                   { params = { Profile = { { name = profile } } } })
end

-- --- Bluetooth takes over --------------------------------------------------

setup()
local meta = wp.add("metadata", wp.object({ ["metadata.name"] = "default" },
    { values = { ["default.configured.audio.sink"] = '{"name":"droid-sink"}' } }))
wp.add("device", droid_card("default"))
bt_connects(wp.object({ ["node.name"] = "bluez_output.x" }))
T.check("the pinned default is dropped when a headset connects",
        meta.values["default.configured.audio.sink"] == nil)

-- During a call the pin stays: callaudiod put it there for a reason, and the
-- voice path of a headset does not run through this host at all.
setup()
meta = wp.add("metadata", wp.object({ ["metadata.name"] = "default" },
    { values = { ["default.configured.audio.sink"] = '{"name":"droid-sink"}' } }))
wp.add("device", droid_card("voicecall"))
bt_connects(wp.object({ ["node.name"] = "bluez_output.x" }))
T.check("but not during a call",
        meta.values["default.configured.audio.sink"] ~= nil)

-- Nothing pinned: priority is already deciding, so there is nothing to do.
setup()
meta = wp.add("metadata", wp.object({ ["metadata.name"] = "default" }))
wp.add("device", droid_card("default"))
bt_connects(wp.object({ ["node.name"] = "bluez_output.x" }))
T.check_equal("with nothing pinned it does not write", 0, #wp.calls_of("metadata_set"))

-- No metadata object at all.
setup()
wp.add("device", droid_card("default"))
bt_connects(wp.object({ ["node.name"] = "bluez_output.x" }))
T.check_equal("without metadata it does nothing", 0, #wp.calls_of("metadata_set"))

-- No droid card: inCall() has to answer "no" rather than fail.
setup()
meta = wp.add("metadata", wp.object({ ["metadata.name"] = "default" },
    { values = { ["default.configured.audio.sink"] = '{"name":"x"}' } }))
bt_connects(wp.object({ ["node.name"] = "bluez_output.x" }))
T.check("with no phone card at all it still drops the pin",
        meta.values["default.configured.audio.sink"] == nil)

-- --- the VoIP path never becomes the default -------------------------------

setup()
meta = wp.add("metadata", wp.object({ ["metadata.name"] = "default" }))
wp.add("node", wp.object({ ["node.name"] = "droid-sink" }))
default_sink_changed("droid-voip-sink")
T.check("a default landing on the VoIP path is moved back",
        (meta.values["default.configured.audio.sink"] or ""):find("droid-sink", 1, true) ~= nil)

setup()
meta = wp.add("metadata", wp.object({ ["metadata.name"] = "default" }))
wp.add("node", wp.object({ ["node.name"] = "droid-sink" }))
default_sink_changed("droid-sink")
T.check_equal("an ordinary default is left alone", 0, #wp.calls_of("metadata_set"))

-- The VoIP node is the default but the phone sink is gone - nothing to move to.
setup()
wp.add("metadata", wp.object({ ["metadata.name"] = "default" }))
default_sink_changed("droid-voip-source")
T.check_equal("with no phone sink to move to, nothing is written", 0,
              #wp.calls_of("metadata_set"))

setup()
local hook = wp.hooks["node/droid-voip-never-default"]
T.traced(function () hook.execute({ get_properties = function () return {} end }) end)
T.check_equal("an event without a value is ignored", 0, #wp.calls_of("metadata_set"))
T.traced(function ()
  hook.execute({ get_properties = function ()
    return { ["event.subject.value"] = "not json" }
  end })
end)
T.check_equal("and so is one that does not parse", 0, #wp.calls_of("metadata_set"))

T.done()
