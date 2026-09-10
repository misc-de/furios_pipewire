-- SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
-- SPDX-License-Identifier: MIT
-- Put a phone call on the Bluetooth headset without anyone typing anything.
--
-- On this device the voice path of a mobile call never reaches the host: it
-- runs modem <-> DSP. A headset is served by the HAL instead, which puts the
-- path onto the Bluetooth PCM line when it is routed to a BT SCO device
-- (set_parameters BT_SCO=on). Two things have to be true at once for that:
--
--   1. the headset is in a hands-free profile, so an SCO channel exists at all
--   2. the phone card is routed to output-bluetooth_sco / input-bluetooth_sco_headset
--
-- "audioctl bt-call watch" does both, but it has to be started before the call.
-- This does the same as a hook: it reacts when the card enters the voicecall
-- profile, and puts everything back when the call ends.
--
-- callaudiod resets the port during a call - it only knows earpiece and
-- speaker - so the route is set again whenever it moves away.
--
-- Everything here is wrapped in pcall. This runs inside the monitor, and an
-- error would take the whole script down with it: no card, no nodes, no sound.
-- A call that stays on the earpiece is a nuisance; a call with no audio at all
-- is not, and the phone must survive whatever this gets wrong.

cutils = require ("common-utils")
log = Log.open_topic ("s-node")

BT_SINK_ROUTE   = "output-bluetooth_sco"
BT_SOURCE_ROUTE = "input-bluetooth_sco_headset"

in_bt_call = false
saved_routes = nil

function btCard ()
  return cutils.get_object_manager ("device"):lookup {
    Constraint { "device.api", "=", "bluez5" },
  }
end

function inVoiceCall (dev)
  for p in dev:iterate_params ("Profile") do
    local profile = cutils.parseParam (p, "Profile")
    if profile and profile.name == "voicecall" then
      return true
    end
  end
  return false
end

-- Route names of what is active right now, per direction.
function activeRoutes (dev)
  local out = {}
  for p in dev:iterate_params ("Route") do
    local r = cutils.parseParam (p, "Route")
    if r and r.device ~= nil and r.name then
      out[r.device] = r.name
    end
  end
  return out
end

function setRouteByName (dev, name)
  for p in dev:iterate_params ("EnumRoute") do
    local r = cutils.parseParam (p, "EnumRoute")
    if r and r.name == name and r.devices ~= nil then
      for _, d in ipairs (r.devices) do
        dev:set_params ("Route", Pod.Object {
          "Spa:Pod:Object:Param:Route", "Route",
          index = r.index,
          device = d,
          -- Never stored: this is where the call goes, not a choice the owner
          -- made about their phone.
          save = false,
        })
      end
      return true
    end
  end
  return false
end

-- The headset has to be in a hands-free profile, otherwise there is no SCO
-- channel for the HAL to put the voice path on.
function setBtProfile (card, name)
  if card == nil then
    return false
  end
  for p in card:iterate_params ("EnumProfile") do
    local profile = cutils.parseParam (p, "EnumProfile")
    if profile and profile.name == name then
      card:set_params ("Profile", Pod.Object {
        "Spa:Pod:Object:Param:Profile", "Profile",
        index = profile.index,
        save = false,
      })
      return true
    end
  end
  return false
end

function enterBtCall (dev, card)
  saved_routes = activeRoutes (dev)
  in_bt_call = true

  local prof = setBtProfile (card, "headset-head-unit")
             or setBtProfile (card, "headset-head-unit-cvsd")
  local sink = setRouteByName (dev, BT_SINK_ROUTE)
  local src  = setRouteByName (dev, BT_SOURCE_ROUTE)

  log:info (string.format (
      "bluetooth call: headset profile %s, output %s, input %s",
      prof and "set" or "NOT set", sink and "set" or "NOT set",
      src and "set" or "NOT set"))

  if not sink then
    -- Without the output route the HAL never sends BT_SCO=on, and holding a
    -- half-applied state would be worse than not trying.
    in_bt_call = false
    saved_routes = nil
  end
end

function leaveBtCall (dev, card)
  in_bt_call = false
  if saved_routes then
    for _, name in pairs (saved_routes) do
      setRouteByName (dev, name)
    end
    saved_routes = nil
  end
  setBtProfile (card, "a2dp-sink")
  log:info ("bluetooth call: over, back to the phone")
end

-- callaudiod sets the port back to the earpiece during the call. Put it back.
function defendRoute (dev)
  local active = activeRoutes (dev)
  for _, name in pairs (active) do
    if name == BT_SINK_ROUTE then
      return
    end
  end
  log:info ("bluetooth call: route moved away, setting it again")
  setRouteByName (dev, BT_SINK_ROUTE)
  setRouteByName (dev, BT_SOURCE_ROUTE)
end

bluetooth_call_hook = SimpleEventHook {
  name = "monitor/droid-bluetooth-call",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "device-params-changed" },
      Constraint { "event.subject.param-id", "=", "Profile" },
    },
    EventInterest {
      Constraint { "event.type", "=", "device-params-changed" },
      Constraint { "event.subject.param-id", "=", "Route" },
    },
  },
  execute = function (event)
    local dev = event:get_subject ()
    if dev.properties["device.api"] ~= "droid-hal" then
      return
    end

    local ok, err = pcall (function ()
      local call = inVoiceCall (dev)
      local card = btCard ()

      if call and not in_bt_call then
        -- No headset, nothing to do: the call stays on the phone, which is
        -- exactly right.
        if card ~= nil then
          enterBtCall (dev, card)
        end
      elseif not call and in_bt_call then
        leaveBtCall (dev, card)
      elseif call and in_bt_call then
        defendRoute (dev)
      end
    end)
    if not ok then
      in_bt_call = false
      saved_routes = nil
      log:warning ("bluetooth call: giving up - " .. tostring (err))
    end
  end,
}

bluetooth_call_hook:register ()
