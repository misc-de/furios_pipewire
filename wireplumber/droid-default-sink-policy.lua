-- SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
-- SPDX-License-Identifier: MIT
-- Two rules about which sink the audio goes to by default.
--
-- 1. A Bluetooth speaker that just connected should get it.
--
-- It does not on its own, and priority is not the reason: the Bluetooth sink
-- ranks at 1010 against the phone's 1000, so it would win - if anything were
-- still deciding by priority. callaudiod pins the phone card as the configured
-- default the first time it runs (SET_DEFAULT_SINK, which happens on the first
-- call or ringtone), and a configured choice weighs 30000 internally. From then
-- on no device can take over by itself, no matter what it connects.
--
-- So when a Bluetooth sink appears, the pin is dropped rather than replaced:
-- priority decides again, the headset wins while it is connected, and the phone
-- takes over the moment it is gone. Replacing the pin with the headset would
-- only move the problem - the next device would be locked out the same way.
--
-- Not during a call. callaudiod pins the phone card for a reason there, and the
-- voice path of a headset does not run through this host at all (see
-- 51-bluez-ofono.conf); moving the default mid-call would take the audio to a
-- device that cannot carry it. "audioctl bt-call" is the way to put a call on a
-- headset.

-- 2. The VoIP nodes should never get it. They exist for the HAL's voice path
-- and nothing should land there by accident - it does not reach the speaker.
-- Low priority is not enough: WirePlumber keeps earlier choices as a fallback
-- chain and walks it when a device disappears, and once a VoIP node has been
-- in that chain it can be picked again. Measured: unplugging a headset mid
-- playback left the default on droid-voip-sink and the phone silent.

cutils = require ("common-utils")
log = Log.open_topic ("s-node")

function inCall ()
  local dev = cutils.get_object_manager ("device"):lookup {
    Constraint { "device.api", "=", "droid-hal" },
  }
  if not dev then
    return false
  end
  for p in dev:iterate_params ("Profile") do
    local profile = cutils.parseParam (p, "Profile")
    if profile and profile.name == "voicecall" then
      return true
    end
  end
  return false
end

bluetooth_takes_over_hook = SimpleEventHook {
  name = "node/droid-bluetooth-takes-over",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "node-added" },
      Constraint { "media.class", "=", "Audio/Sink" },
      Constraint { "device.api", "=", "bluez5", type = "pw" },
    },
  },
  execute = function (event)
    local node = event:get_subject ()
    local name = node.properties ["node.name"]

    if inCall () then
      log:info ("bluetooth takes over: in a call, leaving the default alone")
      return
    end

    local m = cutils.get_object_manager ("metadata"):lookup {
      Constraint { "metadata.name", "=", "default" },
    }
    if not m then
      return
    end

    local cur = m:find (0, "default.configured.audio.sink")
    if cur == nil or cur == "" then
      return    -- nothing pinned, priority is already deciding
    end

    log:info ("bluetooth takes over: " .. tostring (name) ..
              " connected, dropping the pinned default (" .. tostring (cur) .. ")")
    -- Remove the key, do not write an empty string into it: an empty value
    -- still claims to be Spa:String:JSON, and whoever reads it next trips over
    -- "wp_spa_json_object_get_valist: assertion 'wp_spa_json_is_object' failed".
    m:set (0, "default.configured.audio.sink", nil, nil)
  end,
}

bluetooth_takes_over_hook:register ()

voip_never_default_hook = SimpleEventHook {
  name = "node/droid-voip-never-default",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "metadata-changed" },
      Constraint { "metadata.name", "=", "default" },
      Constraint { "event.subject.key", "=", "default.audio.sink" },
    },
  },
  execute = function (event)
    local raw = event:get_properties () ["event.subject.value"]
    if not raw then
      return
    end
    local ok, parsed = pcall (function () return Json.Raw (raw):parse () end)
    local name = ok and parsed and parsed.name or nil
    if not name or not string.find (name, "droid-voip", 1, true) then
      return
    end

    local fallback = cutils.get_object_manager ("node"):lookup {
      Constraint { "node.name", "=", "droid-sink" },
    }
    local m = cutils.get_object_manager ("metadata"):lookup {
      Constraint { "metadata.name", "=", "default" },
    }
    if not fallback or not m then
      return
    end
    log:info ("default sink landed on " .. name .. " - moving it back to droid-sink")
    m:set (0, "default.configured.audio.sink", "Spa:String:JSON",
           Json.Object { name = "droid-sink" }:to_string ())
  end,
}

voip_never_default_hook:register ()
