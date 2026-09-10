-- The microphone should follow the output choice.
--
-- Pick a headset as the output and the microphone would otherwise stay on the
-- phone: input and output are independent in PipeWire, and callaudiod pins
-- both to the phone card as soon as it detects it (SET_DEFAULT_SINK and
-- SET_DEFAULT_SOURCE). A configured choice weighs 30000 internally and beats
-- any priority - so the headset microphone would never get its turn, even
-- though at 2010 it ranks above the phone's 1000.
--
-- This rule only applies when both ends belong to the same device: pick the
-- headset and its microphone is taken as well. For devices without a
-- microphone nothing happens.

cutils = require ("common-utils")
log = Log.open_topic ("s-node")

function nodeByName (name)
  return cutils.get_object_manager ("node"):lookup {
    Constraint { "node.name", "=", name },
  }
end

function defaultMetadata ()
  return cutils.get_object_manager ("metadata"):lookup {
    Constraint { "metadata.name", "=", "default" },
  }
end

input_follows_output_hook = SimpleEventHook {
  name = "node/droid-input-follows-output",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "metadata-changed" },
      Constraint { "metadata.name", "=", "default" },
      Constraint { "event.subject.key", "=", "default.audio.sink" },
    },
  },
  execute = function (event)
    local props = event:get_properties ()
    local raw = props ["event.subject.value"]
    if not raw then
      return
    end

    local ok, parsed = pcall (function () return Json.Raw (raw):parse () end)
    local name = ok and parsed and parsed.name or nil
    if not name then
      return
    end

    local sink = nodeByName (name)
    local dev = sink and sink.properties ["device.id"]
    if not dev then
      log:debug ("input follows output: " .. name .. " belongs to no device")
      return
    end

    local source = cutils.get_object_manager ("node"):lookup {
      Constraint { "media.class", "=", "Audio/Source" },
      Constraint { "device.id", "=", dev, type = "pw" },
    }
    if not source then
      log:debug ("input follows output: " .. name .. " has no microphone")
      return
    end

    local src_name = source.properties ["node.name"]
    local m = defaultMetadata ()
    if not m then
      return
    end

    -- Do nothing if it is already correct - otherwise this and callaudiod
    -- keep triggering each other.
    local cur = m:find (0, "default.configured.audio.source")
    if cur and string.find (cur, src_name, 1, true) then
      return
    end

    log:info ("input follows output: " .. src_name .. " (because of " .. name .. ")")
    m:set (0, "default.configured.audio.source", "Spa:String:JSON",
           Json.Object { name = src_name }:to_string ())
  end
}

input_follows_output_hook:register ()
