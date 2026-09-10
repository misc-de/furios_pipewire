-- SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
-- SPDX-License-Identifier: MIT
-- The rule that makes the microphone follow the chosen output.
local T = dofile((os.getenv("TEST_ROOT") or ".") .. "/tests/lua/harness.lua")
local wp = T.wp

T.suite("input follows output")

local function setup()
  wp.install()
  T.load_script(T.root .. "/wireplumber/droid-input-follows-output.lua")
end

local function fire(sink_name)
  local hook = wp.hooks["node/droid-input-follows-output"]
  T.traced(function ()
    hook.execute({
      get_properties = function ()
        return { ["event.subject.value"] = '{"name":"' .. sink_name .. '"}' }
      end,
    })
  end)
end

-- A headset with both ends: the microphone should come along.
setup()
local meta = wp.add("metadata", wp.object({ ["metadata.name"] = "default" }))
wp.add("node", wp.object({ ["node.name"] = "usb-sink", ["device.id"] = "7" }))
wp.add("node", wp.object({ ["node.name"] = "usb-mic", ["device.id"] = "7",
                           ["media.class"] = "Audio/Source" }))
wp.add("device", wp.object({ ["bound-id"] = "7", ["device.api"] = "alsa" }))
fire("usb-sink")
T.check("the microphone of the chosen device is taken",
        meta.values["default.configured.audio.source"] ~= nil)
T.check("and it is that device's microphone",
        (meta.values["default.configured.audio.source"] or ""):find("usb-mic", 1, true) ~= nil)

-- The same again: nothing should be written a second time, or this and
-- callaudiod keep setting the default back and forth.
wp.reset()
fire("usb-sink")
T.check_equal("setting the same source again is skipped", 0,
              #wp.calls_of("metadata_set"))

-- Bluetooth: the source exists and delivers silence, so the microphone stays.
setup()
meta = wp.add("metadata", wp.object({ ["metadata.name"] = "default" }))
wp.add("node", wp.object({ ["node.name"] = "bluez_output.x", ["device.id"] = "9" }))
wp.add("node", wp.object({ ["node.name"] = "bluez_input.x", ["device.id"] = "9",
                           ["media.class"] = "Audio/Source",
                           ["bluez5.loopback"] = "true" }))
wp.add("device", wp.object({ ["bound-id"] = "9", ["device.api"] = "bluez5" }))
fire("bluez_output.x")
T.check_equal("a Bluetooth microphone is not taken", 0,
              #wp.calls_of("metadata_set"))

-- A sink that belongs to no device at all.
setup()
wp.add("metadata", wp.object({ ["metadata.name"] = "default" }))
wp.add("node", wp.object({ ["node.name"] = "null-sink" }))
fire("null-sink")
T.check_equal("a sink without a device changes nothing", 0,
              #wp.calls_of("metadata_set"))

-- A device with an output and no input.
setup()
wp.add("metadata", wp.object({ ["metadata.name"] = "default" }))
wp.add("node", wp.object({ ["node.name"] = "speaker-only", ["device.id"] = "3" }))
wp.add("device", wp.object({ ["bound-id"] = "3", ["device.api"] = "alsa" }))
fire("speaker-only")
T.check_equal("a device without a microphone changes nothing", 0,
              #wp.calls_of("metadata_set"))

-- Malformed input: the event carries something that is not the JSON expected.
setup()
wp.add("metadata", wp.object({ ["metadata.name"] = "default" }))
local hook = wp.hooks["node/droid-input-follows-output"]
T.traced(function ()
  hook.execute({ get_properties = function () return {} end })
end)
T.check_equal("an event without a value is ignored", 0, #wp.calls_of("metadata_set"))
T.traced(function ()
  hook.execute({ get_properties = function ()
    return { ["event.subject.value"] = "not json at all" }
  end })
end)
T.check_equal("and so is one that does not parse", 0, #wp.calls_of("metadata_set"))

-- No default metadata at all - nothing to write the choice into.
setup()
wp.add("node", wp.object({ ["node.name"] = "usb-sink", ["device.id"] = "7" }))
wp.add("node", wp.object({ ["node.name"] = "usb-mic", ["device.id"] = "7",
                           ["media.class"] = "Audio/Source" }))
wp.add("device", wp.object({ ["bound-id"] = "7", ["device.api"] = "alsa" }))
fire("usb-sink")
T.check_equal("without metadata nothing is written", 0, #wp.calls_of("metadata_set"))

T.done()
