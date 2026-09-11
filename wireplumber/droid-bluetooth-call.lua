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
-- THIS IS OFF BY DEFAULT, and the reason is a real call that had no audio at
-- all - not on the headset, not on the earpiece, not in either direction.
-- callaudiod resets the port during a call because it only knows earpiece and
-- speaker; this script set it back; callaudiod set it again. Two to three
-- round trips a second, and the HAL tore the voice path down and rebuilt it
-- every time (BT_SCO=off, BT_SCO=on, ...). Picking the earpiece by hand did
-- not help either, because this script overrode that too.
--
-- So: the automatic routing only runs when the setting
-- furios.bluetooth-call-routing is on (see 51-bluez-ofono.conf), and even
-- then it gives up after a few rounds of that fight and hands the call back
-- to the phone. A call on the earpiece is a nuisance. A call with no audio is
-- not, and the phone has to keep working.
--
-- Everything here is wrapped in pcall. This runs inside the monitor, and an
-- error would take the whole script down with it: no card, no nodes, no sound.
-- A call that stays on the earpiece is a nuisance; a call with no audio at all
-- is not, and the phone must survive whatever this gets wrong.

cutils = require ("common-utils")
log = Log.open_topic ("s-node")

BT_SINK_ROUTE   = "output-bluetooth_sco"
BT_SOURCE_ROUTE = "input-bluetooth_sco_headset"

SETTING = "furios.bluetooth-call-routing"

-- How often the route may be pushed away before this gives up for the rest of
-- the call. Three is enough to survive callaudiod setting the port once or
-- twice while the call is being set up, and short enough that a fight is over
-- in well under a second.
MAX_DEFENDS = 3

-- How long the card has to be QUIET before touching the Bluetooth side.
--
-- callaudiod runs a sequence of PulseAudio operations when a call starts -
-- park the output, set the real port, set the input port - and it waits for
-- each one. Changing the route or the Bluetooth profile in the middle of that
-- changes the cards underneath it, and its next SelectMode then blocks until
-- the D-Bus timeout: 25 seconds with no audio in either direction.
--
-- A fixed delay from the first sight of the call is not enough, and a real
-- call showed why: the profile goes to voicecall while the phone is still
-- ringing, so a delay started there elapsed in the middle of callaudiod's
-- work. Measured: the takeover landed 14 ms before callaudiod set the port.
--
-- So the wait is not for a span of time but for quiet. Every route or profile
-- change while the call is being set up starts it again, and the takeover
-- happens only when nothing has moved for this long. callaudiod's own
-- sequence is about 200 ms end to end, so a second of silence means it is
-- finished.
QUIET_MS = 1000

in_bt_call = false
gave_up = false
defends = 0
saved_routes = nil

-- Bumped by every event while we are waiting. A timer that fires with a stale
-- number knows something moved after it was armed, and stands down.
quiet_token = 0
took_over = false

-- Off unless someone turned it on. An unknown setting, an older WirePlumber,
-- anything unexpected: all of that has to come out as "leave the call alone".
function autoRoutingEnabled ()
  local ok, value = pcall (function ()
    return Settings.get_boolean (SETTING)
  end)
  return ok and value == true
end

function btCard ()
  return cutils.get_object_manager ("device"):lookup {
    Constraint { "device.api", "=", "bluez5" },
  }
end

function droidCard ()
  return cutils.get_object_manager ("device"):lookup {
    Constraint { "device.api", "=", "droid-hal" },
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

function takeOver (dev, card)
  took_over = true
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

function enterBtCall (dev, card)
  saved_routes = activeRoutes (dev)
  in_bt_call = true
  took_over = false
  defends = 0
  waitForQuiet (dev, card)
end

-- Arm the takeover, and re-arm it whenever anything moves.
function waitForQuiet (dev, card)
  quiet_token = quiet_token + 1
  local mine = quiet_token

  Core.timeout_add (QUIET_MS, function ()
    -- Something moved after this timer was armed, or the call ended: either
    -- way this one is not the timer that acts.
    if mine ~= quiet_token or not in_bt_call then
      return false
    end
    local ok, err = pcall (function () takeOver (dev, card) end)
    if not ok then
      in_bt_call = false
      saved_routes = nil
      log:warning ("bluetooth call: giving up - " .. tostring (err))
    end
    return false
  end)
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

-- Hand the call back to the phone and do not try again until it is over.
function giveUp (dev, card, why)
  -- Say what was on the route instead. When this fires, the interesting
  -- question is who keeps taking it away, and the name is the only clue there
  -- is from inside here.
  local found = {}
  for _, name in pairs (activeRoutes (dev)) do
    table.insert (found, name)
  end
  log:warning ("bluetooth call: " .. why .. " (route is on " ..
               (next (found) and table.concat (found, ", ") or "nothing") ..
               ") - the call goes back to the phone")
  in_bt_call = false
  gave_up = true
  defends = 0
  if saved_routes then
    for _, name in pairs (saved_routes) do
      setRouteByName (dev, name)
    end
    saved_routes = nil
  end
  setBtProfile (card, "a2dp-sink")
end

-- callaudiod sets the port back to the earpiece during the call. Put it back -
-- but not forever. callaudiod does not yield, and a route that changes two or
-- three times a second means the HAL rebuilds the voice path just as often;
-- the call then carries no audio in either direction, which is worse than a
-- call that stayed on the phone.
function defendRoute (dev, card)
  local active = activeRoutes (dev)
  for _, name in pairs (active) do
    if name == BT_SINK_ROUTE then
      return
    end
  end

  -- Counted on every attempt, not on every observed loss. Reading the card and
  -- acting on what it said is a race against the other side setting it back,
  -- and losing that race meant the count stayed at zero while the two of us
  -- traded the route several times a second - which is exactly the state this
  -- is here to end.
  defends = defends + 1
  if defends > MAX_DEFENDS then
    giveUp (dev, card, "the route will not stay on the headset")
    return
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
    local api = dev.properties["device.api"]

    -- A headset that is connected while the call is already running.
    --
    -- The phone card emits nothing at all in that case - its profile and its
    -- routes are exactly as they were - so waiting for it means waiting
    -- forever. The Bluetooth card, on the other hand, announces itself as it
    -- settles, and that is the only moment there is to notice. Everything
    -- after this point works on the phone card, as before.
    if api == "bluez5" then
      dev = droidCard ()
      if dev == nil then
        return
      end
    elseif api ~= "droid-hal" then
      return
    end

    local ok, err = pcall (function ()
      local call = inVoiceCall (dev)
      local card = btCard ()

      if call and not in_bt_call and not gave_up then
        -- No headset, nothing to do: the call stays on the phone, which is
        -- exactly right. Same when the automatic routing is switched off.
        if card ~= nil and autoRoutingEnabled () then
          enterBtCall (dev, card)
        end
      elseif not call and in_bt_call then
        leaveBtCall (dev, card)
      elseif not call and gave_up then
        -- Already handed back mid-call; the call is over, so try again next
        -- time.
        gave_up = false
      elseif call and in_bt_call then
        if saved_routes ~= nil and not took_over then
          -- Still waiting for callaudiod to finish: anything that moves
          -- starts the wait again.
          waitForQuiet (dev, card)
        else
          defendRoute (dev, card)
        end
      end
    end)
    if not ok then
      in_bt_call = false
      gave_up = false
      defends = 0
      saved_routes = nil
      log:warning ("bluetooth call: giving up - " .. tostring (err))
    end
  end,
}

bluetooth_call_hook:register ()
